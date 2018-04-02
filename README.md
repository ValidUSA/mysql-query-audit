# MySQL Query Audit Plugin

[![Build Status](https://travis-ci.org/ValidUSA/mysql-query-audit.svg?branch=master)](https://travis-ci.org/ValidUSA/mysql-query-audit)

This plugin is designed to log read access such as ``SELECT`` and ``JOIN`` operations against a list of configured tables. It is currently only tested against MySQL Community 5.7 on CentOS 7.

This plugin was created because:
* The [MariaDB Server Audit Plugin](https://github.com/MariaDB/server/tree/10.3/plugin/server_audit) logs too much information and doesn't have configuration options to tune it to our needs. Log file sizes quickly get out of hand.
* [McAffee's mysql-audit plugin](https://github.com/mcafee/mysql-audit) requires [SELinux to be disabled](https://github.com/mcafee/mysql-audit/wiki/Troubleshooting#plugin-fails-to-load-with-hot-patching-problems) which is [not acceptable](https://stopdisablingselinux.com/) in our environment.
* Oracle's Enterprise MySQL _starts_ at $5k/year and would require us to reprovision all our DB instances.

## :zap: Under Active Development :volcano:
This plugin is still under development. You should anticipate significant changes to the JSON log format and global variable names before an official release is created.

Pull requests to support other operating systems or MariaDB are welcome but we may not be able to put any significant time or effort into older versions of MySQL.

## Log File Format
By default logs are written to: ``/var/log/query_audit.log``. Logs are saved as newline delimited JSON. For example:

### type = "query"
Table access queries are logged as ``"type": "query"``:

```
{"time": "2018-03-07 09:53:32", "type": "query", "user": "user@host", "connection": 123, "db": "dbname", "table": "tablename", "query": "... FULL QUERY ..."}
```

### type = "global_var"
Any changes to the plugin's variables are logged as ``"type": "set_global"``:

```
{"time":"2018-04-02 07:26:23", "type":"set_global", "user":"user@host", "connection": 123, "variable":"query_audit_log_databases", "value":"inventory,wiki"}
```

## Installation

You can [download the latest development release from Manta](https://us-east.manta.joyent.com/tpaul/public/dev/mysql/query_audit/latest/query_audit.so) or you can follow the [Build Instructions](#build-instructions) to create your own copy.

```
# Download or copy the plugin to the server:
curl -O https://us-east.manta.joyent.com/tpaul/public/dev/mysql/query_audit/latest/query_audit.so

# Install the plugin to the mysql plugin directory:
sudo install -g root -o root -m 0755 query_audit.so /usr/lib64/mysql/plugin

# Set the SELinux context:
sudo chcon -u system_u -r object_r -t lib_t /usr/lib64/mysql/plugin/query_audit.so

# Create the log file:
sudo touch /var/log/query_audit.log

# Set owner, pemissions, and SELinux context:
sudo chown mysql:mysql /var/log/query_audit.log
sudo chmod 0640 /var/log/query_audit.log
sudo chcon -u system_u -r object_r -t mysqld_log_t /var/log/query_audit.log

# Install the plugin:
echo "INSTALL PLUGIN query_audit SONAME 'query_audit.so';" | mysql -u root -p
```

If you ever need to update the plugin you must first uninstall the old version:

```
echo "UNINSTALL PLUGIN query_audit;" | mysql -u root -p
```

## Configuring the Plugin

Global configuration variables for the plugin can be set at runtime using ``SET GLOBAL VAR=VALUE`` and can be made persistent by adding them to the my.cnf configuration file.

You can get a list of all variables for this plugin and their values with the following command:

```
SHOW GLOBAL VARIABLES LIKE 'query_audit%';
```

## Configuration Variables

### query\_audit\_log\_file

This variable must contain an absolute path to the log file. By design, the plugin (and thus, MySQL) will **NOT** start if this file cannot be opened or created.

**my.cnf (persistent):**

```
query_audit_log_file=/var/log/query_audit.log
```

**SQL (runtime):**

```
SET GLOBAL query_audit_log_file='/var/log/query_audit_2.log';
```

### query\_audit\_always\_fflush

When this variable is set to ``on`` or ``1`` the plugin will call [fflush(3)](http://man7.org/linux/man-pages/man3/fflush.3.html) after writing to the file. This may incur a minor a performance penalty, but it ensures that user space buffers are flushed to disk.

**my.cnf (persistent):**

```
query_audit_always_fflush=on
```

**SQL (runtime):**

```
SET GLOBAL query_audit_always_fflush=1;
```

### query\_audit\_log\_databases

This variable should contain a comma-separated list of database names. The databases set in this variable will be monitored for read access. If there is also a matching table name in the ``query_audit_log_tables`` variable, the query will be logged.

**my.cnf (persistent):**

```
query_audit_log_databases=inventory,wiki
```

**SQL (runtime):**

```
SET GLOBAL query_audit_log_databases='inventory,wiki';
```

### query\_audit\_log\_tables

This variable should contain a comma-separated list of table names.  You must also configure a matching database name in the ``query_audit_log_databases`` variable.

**my.cnf (persistent):**

```
query_audit_log_tables=snacks,kinds_of_cheese,things_beau_says
```

**SQL (runtime):**

```
SET GLOBAL query_audit_log_tables='snacks,kinds_of_cheese,things_beau_says';
```

## Build Instructions

You need to download a copy of the MySQL Server source code, copy the query\_audit plugin source code directory into ``/path/to/mysql-source/plugin``, then build MySQL with the same configuration as the production instance. This process will produce a shared object file ([query_audit.so]((https://us-east.manta.joyent.com/tpaul/public/dev/mysql/query_audit/latest/query_audit.so))) that can be installed into a running instance.

### Build dependencies


```
sudo yum install yum-utils rpmdevtools cmake make gcc gcc-c++ bison libaio-devel numactl-devel ncurses-devel
```

### Obtaining the MySQL Source

```
sudo rpm -i http://dev.mysql.com/get/mysql57-community-release-el7-7.noarch.rpm
yumdownloader --nogpgcheck --source mysql-community-server
rpmdev-extract mysql-community-5.7.*.el7.src.rpm
cd mysql-community-5.7.*.el7.src
rpmdev-extract mysql-5.7.*.tar.gz boost_1_*.tar.bz2

```


### Place the query audit plugin source into the MySQL source tree

Copy or checkout the query audit plugin into the MySQL source tree's plugin directory:

```
cd /path/to/mysql-community-5.7.xx-1.el7.src/mysql-5.7.xx/plugin
mkdir query_audit
cp -r /path/to/query_audit/src/* query_audit/
```

### Building MySQL and the plugin

You can now perform a build of the entire MySQL source and the plugin will be built along with it. You will need around 4GB of memory to build MySQL. When building with 2GB of memory I observed the C++ compiler being killed by the OOM killer halfway through the build.

The CMake build options are taken from the spec file of mysql-community-5.7.21-1.el7.src.rpm and the file paths were altered to support a local build.

```
mkdir build
cd build
cmake ../ -DBUILD_CONFIG=mysql_release -DINSTALL_LAYOUT=RPM -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_BOOST=../.. -DWITH_SYSTEMD=1 -DWITH_INNODB_MEMCACHED=1 -DINSTALL_LIBDIR="/lib64/mysql" -DINSTALL_PLUGINDIR="/lib64/mysql/plugin" -DMYSQL_UNIX_ADDR="/var/lib/mysql/mysql.sock" -DMYSQLX_UNIX_ADDR="/var/run/mysqld/mysqlx.sock" -DFEATURE_SET="community" -DWITH_EMBEDDED_SERVER=1 -DWITH_EMBEDDED_SHARED_LIBRARY=1 -DWITH_NUMA=ON

# this will take some time on the first run (easily > 20 mins)
# incremental builds usually take only a few seconds
make

```

The compiled shared object will be located at: ``/path/to/mysql-community-5.7.xx-1.el7.src/mysql-5.7.xx/build/plugin/query_audit/query_audit.so``

You only need the ``query_audit.so`` file. It can simply be copied and installed on the server you wish to audit.