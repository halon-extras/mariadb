## MariaDB client plugin

This function connects to a MariaDB client using the MariaDB Connector/C library (mariadb-connector-c-devel / libmariadb-dev has to be installed separately to build). It supports connection pooling and reconnects. Settings are made using a MariaDB client .cnf file (a sample is provided in this folder).

This plugin exports two HSL functions `mysql_query` and `mysql_escape_string`.

### mysql_escape_string(parameter)

Returns the argument parameter escaped. Safe to be used within a SQL statement.

```
$parameter = mysql_escape_string("my string");
```

### mysql_query(statement)

Execute the SQL statement on the server. A successful query result will return an associative array with an "result" array and the "affected" rows. On error an associative array with "errno", "error" and "sqlstate" will be provided.

```
$result = mysql_query("SELECT * FROM table where column = '".mysql_escape_string($value).'";");
```

### smtpd.yaml

```
plugins:
  - id: mariadb
    path: /opt/halon/plugins/mariadb.so
    config:
      cnf: /path/to/halon.cnf
      pool_size: 32
```

## Build

On CentOS install  and on
