# KrahoDB

KrahoDB is an open-source database designed to support multi-master replication. It is designed on the top of PostgreSQL, providing bidirectional replication, as well as row filtering.

KrahoDB is a PostgreSQL fork. We provide versions based on PostgreSQL 10, 11 and 12. All code is licensed under PostgreSQL license.

![Logo](krahodb.png)

## Installation

The [installation process](https://www.postgresql.org/docs/current/installation.html) is the same as PostgreSQL.

Use one of the branches named `krahodb-X-Y` where `X.Y` is the PostgreSQL version (branch `krahodb-10-10` is based on PostgreSQL `10.10`).

## Usage

Under construction...

## Contributing

Contributions are welcome!

How can you help us?

* open an issue
    - bug report
	- feature request
	- suggestion
	- portability problem
* submit a pull request

Before submitting a particular improvement, open an issue to start a discussion about the feature (specially if it is not a trivial change).

Read the [PostgreSQL Coding Conventions](https://www.postgresql.org/docs/current/source.html). It is what we use for KrahoDB. Also, follow the style of the adjacent code! Remove any spurious whitespace (`git diff --color`). We tend to use underscores or Camelcase (prefer the former). Add useful comments (explain a behavior). Avoid "opens a file" comments.

KrahoDB uses [PostgreSQL license](http://www.postgresql.org/about/licence/). By posting a patch (issue or pull request), you agree that your patch can be distributed under the PostgreSQL license.
