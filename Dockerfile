FROM ubuntu:20.04

WORKDIR /usr/src/app

RUN apt-get -qq update && apt-get -yqq install --no-install-recommends vim curl apt-utils autotools-dev automake  \
    openssl libssl-dev gconf2 firefox \
    make g++ gcc gdb nodejs php7.0-cli php7.0-sqlite python pydb python-tk \
    default-jre default-jdk \
    locales supervisor && rm -rf /var/lib/apt/lists/*

COPY vpl-jail-system .

RUN ./install-vpl-sh

COPY entrypoint.sh /

CMD ["/entrypoint.sh"]
 