#!/bin/bash

# Build script for Dockerfile/Travis-CI

set -e

buildroot=$1

if [ -z "$buildroot" ]; then
    buildroot=/tmp
fi

cd $buildroot/mysql-community-5.7.*.el7.src/mysql-5.7.*/

mkdir build

cd build

cmake ../ -DBUILD_CONFIG=mysql_release -DINSTALL_LAYOUT=RPM -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_BOOST=../.. -DWITH_SYSTEMD=1 -DWITH_INNODB_MEMCACHED=1 -DINSTALL_LIBDIR="/lib64/mysql" -DINSTALL_PLUGINDIR="/lib64/mysql/plugin" -DMYSQL_UNIX_ADDR="/var/lib/mysql/mysql.sock" -DMYSQLX_UNIX_ADDR="/var/run/mysqld/mysqlx.sock" -DFEATURE_SET="community" -DWITH_EMBEDDED_SERVER=1 -DWITH_EMBEDDED_SHARED_LIBRARY=1 -DWITH_NUMA=ON

make

cp plugin/query_audit/query_audit.so /tmp/artifact/