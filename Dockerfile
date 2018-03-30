# This Dockerfile is used to setup a build environment for the plugin

FROM centos:7

COPY src /tmp/query_audit

COPY build.sh /tmp

RUN yum -y -q install yum-utils rpmdevtools cmake make gcc gcc-c++ bison libaio-devel numactl-devel ncurses-devel \
    && rpm -i http://dev.mysql.com/get/mysql57-community-release-el7-7.noarch.rpm

RUN cd /tmp \
    && yumdownloader --nogpgcheck --source mysql-community-server \
    && rpmdev-extract -q mysql-community-5.7.*.el7.src.rpm \
    && cd mysql-community-5.7.*.el7.src \
    && rpmdev-extract -q mysql-5.7.*.tar.gz boost_1_*.tar.bz2

RUN mv /tmp/query_audit /tmp/mysql-community-5.7.*.el7.src/mysql-5.7.*/plugin/ \
    && mkdir /tmp/artifact

CMD ["/tmp/build.sh"]