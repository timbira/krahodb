/*-------------------------------------------------------------------------
 *
 * pgoutput.c
 *		Logical Replication output plugin
 *
 * Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/replication/pgoutput/pgoutput.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"

#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"

#include "replication/logical.h"
#include "replication/logicalproto.h"
#include "replication/logicalrelation.h"
#include "replication/origin.h"
#include "replication/pgoutput.h"

#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/int8.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

static void pgoutput_startup(LogicalDecodingContext *ctx,
							 OutputPluginOptions *opt, bool is_init);
static void pgoutput_shutdown(LogicalDecodingContext *ctx);
static void pgoutput_begin_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn);
static void pgoutput_commit_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_change(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn, Relation rel,
							ReorderBufferChange *change);
static void pgoutput_truncate(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, int nrelations, Relation relations[],
							  ReorderBufferChange *change);
static bool pgoutput_origin_filter(LogicalDecodingContext *ctx,
								   RepOriginId origin_id);

static bool publications_valid;

static List *LoadPublications(List *pubnames);
static void publication_invalidation_cb(Datum arg, int cacheid,
										uint32 hashvalue);

/* Entry in the map used to remember which relation schemas we sent. */
typedef struct RelationSyncEntry
{
	Oid			relid;			/* relation oid */
	bool		schema_sent;	/* did we send the schema? */
	bool		replicate_valid;
	PublicationActions pubactions;
	List		*row_filter;
} RelationSyncEntry;

/* Map used to remember which relation schemas we sent. */
static HTAB *RelationSyncCache = NULL;

static void init_rel_sync_cache(MemoryContext decoding_context);
static RelationSyncEntry *get_rel_sync_entry(PGOutputData *data, Oid relid);
static void rel_sync_cache_relation_cb(Datum arg, Oid relid);
static void rel_sync_cache_publication_cb(Datum arg, int cacheid,
										  uint32 hashvalue);

/*
 * Specify output plugin callbacks
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = pgoutput_startup;
	cb->begin_cb = pgoutput_begin_txn;
	cb->change_cb = pgoutput_change;
	cb->truncate_cb = pgoutput_truncate;
	cb->commit_cb = pgoutput_commit_txn;
	cb->filter_by_origin_cb = pgoutput_origin_filter;
	cb->shutdown_cb = pgoutput_shutdown;
}

static void
parse_output_parameters(List *options, uint32 *protocol_version,
						List **publication_names, List **origin_ids)
{
	ListCell   *lc;
	bool		protocol_version_given = false;
	bool		publication_names_given = false;
	bool		origin_ids_given = false;

	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		Assert(defel->arg == NULL || IsA(defel->arg, String));

		/* Check each param, whether or not we recognize it */
		if (strcmp(defel->defname, "proto_version") == 0)
		{
			int64		parsed;

			if (protocol_version_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			protocol_version_given = true;

			if (!scanint8(strVal(defel->arg), true, &parsed))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid proto_version")));

			if (parsed > PG_UINT32_MAX || parsed < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("proto_version \"%s\" out of range",
								strVal(defel->arg))));

			*protocol_version = (uint32) parsed;
		}
		else if (strcmp(defel->defname, "publication_names") == 0)
		{
			if (publication_names_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			publication_names_given = true;

			if (!SplitIdentifierString(strVal(defel->arg), ',',
									   publication_names))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid publication_names syntax")));
		}
		else if (strcmp(defel->defname, "filter_origins") == 0)
		{
			if (origin_ids_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			origin_ids_given = true;

			if (!string_to_oid_list(strVal(defel->arg), ',',
									   origin_ids))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid filter_origins syntax")));
		}
		else
			elog(ERROR, "unrecognized pgoutput option: %s", defel->defname);
	}
}

/*
 * Initialize this plugin
 */
static void
pgoutput_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				 bool is_init)
{
	PGOutputData *data = palloc0(sizeof(PGOutputData));

	/* Create our memory context for private allocations. */
	data->context = AllocSetContextCreate(ctx->context,
										  "logical replication output context",
										  ALLOCSET_DEFAULT_SIZES);

	ctx->output_plugin_private = data;

	/* This plugin uses binary protocol. */
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

	/*
	 * This is replication start and not slot initialization.
	 *
	 * Parse and validate options passed by the client.
	 */
	if (!is_init)
	{
		/* Parse the params and ERROR if we see any we don't recognize */
		parse_output_parameters(ctx->output_plugin_options,
								&data->protocol_version,
								&data->publication_names,
								&data->origin_ids);

		/* Check if we support requested protocol */
		if (data->protocol_version > LOGICALREP_PROTO_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but we only support protocol %d or lower",
							data->protocol_version, LOGICALREP_PROTO_VERSION_NUM)));

		if (data->protocol_version < LOGICALREP_PROTO_MIN_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but we only support protocol %d or higher",
							data->protocol_version, LOGICALREP_PROTO_MIN_VERSION_NUM)));

		if (list_length(data->publication_names) < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("publication_names parameter missing")));

		/* Init publication state. */
		data->publications = NIL;
		publications_valid = false;
		CacheRegisterSyscacheCallback(PUBLICATIONOID,
									  publication_invalidation_cb,
									  (Datum) 0);

		/* Initialize relation schema cache. */
		init_rel_sync_cache(CacheMemoryContext);
	}
}

/*
 * BEGIN callback
 */
static void
pgoutput_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin(ctx->out, txn);

	if (send_replication_origin)
	{
		char	   *origin;

		/* Message boundary */
		OutputPluginWrite(ctx, false);
		OutputPluginPrepareWrite(ctx, true);

		/*----------
		 * XXX: which behaviour do we want here?
		 *
		 * Alternatives:
		 *	- don't send origin message if origin name not found
		 *	  (that's what we do now)
		 *	- throw error - that will break replication, not good
		 *	- send some special "unknown" origin
		 *----------
		 */
		if (replorigin_by_oid(txn->origin_id, true, &origin))
			logicalrep_write_origin(ctx->out, origin, txn->origin_lsn);
	}

	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT callback
 */
static void
pgoutput_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					XLogRecPtr commit_lsn)
{
	OutputPluginUpdateProgress(ctx);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * Write the relation schema if the current schema hasn't been sent yet.
 */
static void
maybe_send_schema(LogicalDecodingContext *ctx,
				  Relation relation, RelationSyncEntry *relentry)
{
	if (!relentry->schema_sent)
	{
		TupleDesc	desc;
		int			i;

		desc = RelationGetDescr(relation);

		/*
		 * Write out type info if needed.  We do that only for user-created
		 * types.  We use FirstGenbkiObjectId as the cutoff, so that we only
		 * consider objects with hand-assigned OIDs to be "built in", not for
		 * instance any function or type defined in the information_schema.
		 * This is important because only hand-assigned OIDs can be expected
		 * to remain stable across major versions.
		 */
		for (i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (att->attisdropped || att->attgenerated)
				continue;

			if (att->atttypid < FirstGenbkiObjectId)
				continue;

			OutputPluginPrepareWrite(ctx, false);
			logicalrep_write_typ(ctx->out, att->atttypid);
			OutputPluginWrite(ctx, false);
		}

		OutputPluginPrepareWrite(ctx, false);
		logicalrep_write_rel(ctx->out, relation);
		OutputPluginWrite(ctx, false);
		relentry->schema_sent = true;
	}
}

/*
 * Sends the decoded DML over wire.
 */
static void
pgoutput_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				Relation relation, ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;

	Form_pg_class	class_form;
	char			*schemaname;
	char			*tablename;

	if (!is_publishable_relation(relation))
		return;

	relentry = get_rel_sync_entry(data, RelationGetRelid(relation));

	/* First check the table filter */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (!relentry->pubactions.pubinsert)
				return;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if (!relentry->pubactions.pubupdate)
				return;
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (!relentry->pubactions.pubdelete)
				return;
			break;
		default:
			Assert(false);
	}

	class_form = RelationGetForm(relation);
	schemaname = get_namespace_name(class_form->relnamespace);
	tablename = NameStr(class_form->relname);

	if (change->action == REORDER_BUFFER_CHANGE_INSERT)
		elog(DEBUG1, "INSERT \"%s\".\"%s\" txid: %u", schemaname, tablename, txn->xid);
	else if (change->action == REORDER_BUFFER_CHANGE_UPDATE)
		elog(DEBUG1, "UPDATE \"%s\".\"%s\" txid: %u", schemaname, tablename, txn->xid);
	else if (change->action == REORDER_BUFFER_CHANGE_DELETE)
		elog(DEBUG1, "DELETE \"%s\".\"%s\" txid: %u", schemaname, tablename, txn->xid);

	/* ... then check row filter */
	if (list_length(relentry->row_filter) > 0)
	{
		HeapTuple		old_tuple;
		HeapTuple		new_tuple;
		TupleDesc		tupdesc;
		EState			*estate;
		ExprContext		*ecxt;
		MemoryContext	oldcxt;
		ListCell		*lc;

		old_tuple = change->data.tp.oldtuple ? &change->data.tp.oldtuple->tuple : NULL;
		new_tuple = change->data.tp.newtuple ? &change->data.tp.newtuple->tuple : NULL;
		tupdesc = RelationGetDescr(relation);
		estate = create_estate_for_relation(relation);

#ifdef	_NOT_USED
		if (old_tuple)
		{
			int i;

			for (i = 0; i < tupdesc->natts; i++)
			{
				Form_pg_attribute	attr;
				HeapTuple			type_tuple;
				Oid					typoutput;
				bool				typisvarlena;
				bool				isnull;
				Datum				val;
				char				*outputstr = NULL;

				attr = TupleDescAttr(tupdesc, i);

				/* Figure out type name */
				type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attr->atttypid));
				if (HeapTupleIsValid(type_tuple))
				{
					/* Get information needed for printing values of a type */
					getTypeOutputInfo(attr->atttypid, &typoutput, &typisvarlena);

					val = heap_getattr(old_tuple, i + 1, tupdesc, &isnull);
					if (!isnull)
					{
						outputstr = OidOutputFunctionCall(typoutput, val);
						elog(DEBUG2, "row filter: REPLICA IDENTITY %s: %s", NameStr(attr->attname), outputstr);
						pfree(outputstr);
					}
				}
			}
		}
#endif

		/* prepare context per tuple */
		ecxt = GetPerTupleExprContext(estate);
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		ecxt->ecxt_scantuple = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(ecxt->ecxt_scantuple, tupdesc);
		MemoryContextSwitchTo(oldcxt);

		ExecStoreTuple(new_tuple ? new_tuple : old_tuple, ecxt->ecxt_scantuple, InvalidBuffer, false);

		foreach (lc, relentry->row_filter)
		{
			Node		*row_filter;
			ExprState	*expr_state;
			Expr		*expr;
			Oid			expr_type;
			Datum		res;
			bool		isnull;
			char		*s = NULL;

			row_filter = (Node *) lfirst(lc);

			/* evaluates row filter */
			expr_type = exprType(row_filter);
			expr = (Expr *) coerce_to_target_type(NULL, row_filter, expr_type, BOOLOID, -1, COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
			expr = expression_planner(expr);
			expr_state = ExecInitExpr(expr, NULL);
			res = ExecEvalExpr(expr_state, ecxt, &isnull);

			elog(DEBUG3, "row filter: result: %s ; isnull: %s", (DatumGetBool(res)) ? "true" : "false", (isnull) ? "true" : "false");

			/* if tuple does not match row filter, bail out */
			if (!DatumGetBool(res) || isnull)
			{
				s = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, CStringGetTextDatum(nodeToString(row_filter)), ObjectIdGetDatum(relentry->relid)));
				elog(DEBUG2, "row filter \"%s\" was not matched", s);
				pfree(s);
				return;
			}

			s = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, CStringGetTextDatum(nodeToString(row_filter)), ObjectIdGetDatum(relentry->relid)));
			elog(DEBUG2, "row filter \"%s\" was matched", s);
			pfree(s);
		}

		ExecDropSingleTupleTableSlot(ecxt->ecxt_scantuple);
		FreeExecutorState(estate);
	}

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	maybe_send_schema(ctx, relation, relentry);

	/* Send the data */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			OutputPluginPrepareWrite(ctx, true);
			logicalrep_write_insert(ctx->out, relation,
									&change->data.tp.newtuple->tuple);
			OutputPluginWrite(ctx, true);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			{
				HeapTuple	oldtuple = change->data.tp.oldtuple ?
				&change->data.tp.oldtuple->tuple : NULL;

				OutputPluginPrepareWrite(ctx, true);
				logicalrep_write_update(ctx->out, relation, oldtuple,
										&change->data.tp.newtuple->tuple);
				OutputPluginWrite(ctx, true);
				break;
			}
		case REORDER_BUFFER_CHANGE_DELETE:
			if (change->data.tp.oldtuple)
			{
				OutputPluginPrepareWrite(ctx, true);
				logicalrep_write_delete(ctx->out, relation,
										&change->data.tp.oldtuple->tuple);
				OutputPluginWrite(ctx, true);
			}
			else
				elog(DEBUG1, "didn't send DELETE change because of missing oldtuple");
			break;
		default:
			Assert(false);
	}

	/* Cleanup */
	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

static void
pgoutput_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  int nrelations, Relation relations[], ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	int			i;
	int			nrelids;
	Oid		   *relids;

	old = MemoryContextSwitchTo(data->context);

	relids = palloc0(nrelations * sizeof(Oid));
	nrelids = 0;

	for (i = 0; i < nrelations; i++)
	{
		Relation	relation = relations[i];
		Oid			relid = RelationGetRelid(relation);

		if (!is_publishable_relation(relation))
			continue;

		relentry = get_rel_sync_entry(data, relid);

		if (!relentry->pubactions.pubtruncate)
			continue;

		relids[nrelids++] = relid;
		maybe_send_schema(ctx, relation, relentry);
	}

	if (nrelids > 0)
	{
		OutputPluginPrepareWrite(ctx, true);
		logicalrep_write_truncate(ctx->out,
								  nrelids,
								  relids,
								  change->data.truncate.cascade,
								  change->data.truncate.restart_seqs);
		OutputPluginWrite(ctx, true);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

/*
 * Determine whether changes will be filtered or forwarded.
 */
static bool
pgoutput_origin_filter(LogicalDecodingContext *ctx,
					   RepOriginId origin_id)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;

	elog(DEBUG1, "origin: %u", origin_id);

	/* changes produced locally are never filtered */
	if (origin_id == InvalidRepOriginId)
		return false;

	elog(DEBUG3, "filter_origins list length: %d", list_length(data->origin_ids));

	/* changes are only filtered from those origin ids provided by the subscriber */
	if (list_length(data->origin_ids) > 0 && list_member_oid(data->origin_ids, origin_id))
	{
		elog(DEBUG2, "origin filter %u was matched", origin_id);
		return true;
	}

	/* there isn't a list of origins to filter out then forward to all subscribers */
	return false;
}

/*
 * Shutdown the output plugin.
 *
 * Note, we don't need to clean the data->context as it's child context
 * of the ctx->context so it will be cleaned up by logical decoding machinery.
 */
static void
pgoutput_shutdown(LogicalDecodingContext *ctx)
{
	if (RelationSyncCache)
	{
		hash_destroy(RelationSyncCache);
		RelationSyncCache = NULL;
	}
}

/*
 * Load publications from the list of publication names.
 */
static List *
LoadPublications(List *pubnames)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, pubnames)
	{
		char	   *pubname = (char *) lfirst(lc);
		Publication *pub = GetPublicationByName(pubname, false);

		result = lappend(result, pub);
	}

	return result;
}

/*
 * Publication cache invalidation callback.
 */
static void
publication_invalidation_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	publications_valid = false;

	/*
	 * Also invalidate per-relation cache so that next time the filtering info
	 * is checked it will be updated with the new publication settings.
	 */
	rel_sync_cache_publication_cb(arg, cacheid, hashvalue);
}

/*
 * Initialize the relation schema sync cache for a decoding session.
 *
 * The hash table is destroyed at the end of a decoding session. While
 * relcache invalidations still exist and will still be invoked, they
 * will just see the null hash table global and take no action.
 */
static void
init_rel_sync_cache(MemoryContext cachectx)
{
	HASHCTL		ctl;
	MemoryContext old_ctxt;

	if (RelationSyncCache != NULL)
		return;

	/* Make a new hash table for the cache */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelationSyncEntry);
	ctl.hcxt = cachectx;

	old_ctxt = MemoryContextSwitchTo(cachectx);
	RelationSyncCache = hash_create("logical replication output relation cache",
									128, &ctl,
									HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);
	(void) MemoryContextSwitchTo(old_ctxt);

	Assert(RelationSyncCache != NULL);

	CacheRegisterRelcacheCallback(rel_sync_cache_relation_cb, (Datum) 0);
	CacheRegisterSyscacheCallback(PUBLICATIONRELMAP,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);
}

/*
 * Find or create entry in the relation schema cache.
 */
static RelationSyncEntry *
get_rel_sync_entry(PGOutputData *data, Oid relid)
{
	RelationSyncEntry *entry;
	bool		found;
	MemoryContext oldctx;

	Assert(RelationSyncCache != NULL);

	/* Find cached function info, creating if not found */
	oldctx = MemoryContextSwitchTo(CacheMemoryContext);
	entry = (RelationSyncEntry *) hash_search(RelationSyncCache,
											  (void *) &relid,
											  HASH_ENTER, &found);
	MemoryContextSwitchTo(oldctx);
	Assert(entry != NULL);

	/* Not found means schema wasn't sent */
	if (!found || !entry->replicate_valid)
	{
		List	   *pubids = GetRelationPublications(relid);
		ListCell   *lc;

		/* Reload publications if needed before use. */
		if (!publications_valid)
		{
			oldctx = MemoryContextSwitchTo(CacheMemoryContext);
			if (data->publications)
				list_free_deep(data->publications);

			data->publications = LoadPublications(data->publication_names);
			MemoryContextSwitchTo(oldctx);
			publications_valid = true;
		}

		/*
		 * Build publication cache. We can't use one provided by relcache as
		 * relcache considers all publications given relation is in, but here
		 * we only need to consider ones that the subscriber requested.
		 */
		entry->pubactions.pubinsert = entry->pubactions.pubupdate =
			entry->pubactions.pubdelete = entry->pubactions.pubtruncate = false;
		entry->row_filter = NIL;

		foreach(lc, data->publications)
		{
			Publication *pub = lfirst(lc);
			HeapTuple	rf_tuple;
			Datum		rf_datum;
			bool		rf_isnull;

			if (pub->alltables || list_member_oid(pubids, pub->oid))
			{
				entry->pubactions.pubinsert |= pub->pubactions.pubinsert;
				entry->pubactions.pubupdate |= pub->pubactions.pubupdate;
				entry->pubactions.pubdelete |= pub->pubactions.pubdelete;
				entry->pubactions.pubtruncate |= pub->pubactions.pubtruncate;
			}

			/* Cache row filters, if available */
			rf_tuple = SearchSysCache2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid), ObjectIdGetDatum(pub->oid));
			if (HeapTupleIsValid(rf_tuple))
			{
				rf_datum = SysCacheGetAttr(PUBLICATIONRELMAP, rf_tuple, Anum_pg_publication_rel_prrowfilter, &rf_isnull);

				if (!rf_isnull)
				{
					MemoryContext oldctx = MemoryContextSwitchTo(CacheMemoryContext);
					char	*s = TextDatumGetCString(rf_datum);
					char	*t = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, rf_datum, ObjectIdGetDatum(entry->relid)));
					Node	*rf_node = stringToNode(s);
					entry->row_filter = lappend(entry->row_filter, rf_node);
					MemoryContextSwitchTo(oldctx);

					elog(DEBUG2, "row filter \"%s\" found for publication \"%s\" and relation \"%s\"", t, pub->name, get_rel_name(relid));
				}

				ReleaseSysCache(rf_tuple);
			}
		}

		list_free(pubids);

		entry->replicate_valid = true;
	}

	if (!found)
		entry->schema_sent = false;

	return entry;
}

/*
 * Relcache invalidation callback
 */
static void
rel_sync_cache_relation_cb(Datum arg, Oid relid)
{
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelSchemaSyncCache is destroyed when the decoding finishes, but there
	 * is no way to unregister the relcache invalidation callback.
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * Nobody keeps pointers to entries in this hash table around outside
	 * logical decoding callback calls - but invalidation events can come in
	 * *during* a callback if we access the relcache in the callback. Because
	 * of that we must mark the cache entry as invalid but not remove it from
	 * the hash while it could still be referenced, then prune it at a later
	 * safe point.
	 *
	 * Getting invalidations for relations that aren't in the table is
	 * entirely normal, since there's no way to unregister for an invalidation
	 * event. So we don't care if it's found or not.
	 */
	entry = (RelationSyncEntry *) hash_search(RelationSyncCache, &relid,
											  HASH_FIND, NULL);

	/*
	 * Reset schema sent status as the relation definition may have changed.
	 */
	if (entry != NULL)
		entry->schema_sent = false;
}

/*
 * Publication relation map syscache invalidation callback
 */
static void
rel_sync_cache_publication_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelSchemaSyncCache is destroyed when the decoding finishes, but there
	 * is no way to unregister the relcache invalidation callback.
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * There is no way to find which entry in our cache the hash belongs to so
	 * mark the whole cache as invalid.
	 */
	hash_seq_init(&status, RelationSyncCache);
	while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
	{
		entry->replicate_valid = false;
		if (list_length(entry->row_filter) > 0)
			list_free(entry->row_filter);
		entry->row_filter = NIL;
	}
}
