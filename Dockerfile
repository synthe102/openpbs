FROM debian:buster-slim

RUN apt-get update
RUN apt-get install -y gcc make libtool libhwloc-dev libx11-dev \
  libxt-dev libedit-dev libical-dev ncurses-dev perl \
  postgresql-server-dev-all postgresql-contrib python3-dev tcl-dev tk-dev swig \
  libexpat-dev libssl-dev libxext-dev libxft-dev autoconf \
  automake g++ libcjson-dev git

RUN apt-get install -y expat libedit2 postgresql python3 postgresql-contrib sendmail-bin \
  sudo tcl tk libical3 libcjson1

WORKDIR /openpbs

COPY . .

RUN ./autogen.sh
RUN ./configure --prefix=/opt/pbs
RUN make
RUN make install
RUN /opt/pbs/libexec/pbs_postinstall
RUN chmod 4755 /opt/pbs/sbin/pbs_iff /opt/pbs/sbin/pbs_rcp

COPY pbs.conf /etc/pbs.conf
RUN ./autogen.sh
