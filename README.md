# MariaDB client plugin

This plugin connects to a MariaDB database using the MariaDB Connector/C library. It supports connection pooling and reconnects. Settings are made using a MariaDB client .cnf file (a [sample](sample.cnf) is provided in this folder).

## Installation

Follow the [instructions](https://docs.halon.io/manual/comp_install.html#installation) in our manual to add our package repository and then run the below command.

### Ubuntu

```
apt-get install halon-extras-mariadb
```

### RHEL

```
yum install halon-extras-mariadb
```

## Configuration
For the configuration schema, see [mariadb.schema.json](mariadb.schema.json). Below is a sample configuration.

```
plugins:
  - id: mariadb
    config:
      cnf: /path/to/halon.cnf
      pool_size: 32
```

## Exported functions

### mysql_escape_string(parameter)

Returns the argument parameter escaped. Safe to be used within a SQL statement.

```
import { mysql_escape_string } from "extras://mariadb";
$parameter = mysql_escape_string("my string");
```

### mysql_query(statement)

Execute the SQL statement on the server. A successful query result will return an associative array with an "result" array and the "affected" rows. On error an associative array with "errno", "error" and "sqlstate" will be provided.

```
import { mysql_query, mysql_escape_string } from "extras://mariadb";
$result = mysql_query("SELECT * FROM table where column = '".mysql_escape_string($value)."';");
```
