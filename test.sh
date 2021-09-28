#!/bin/bash

set -eo pipefail

#apt-get update
#apt-get install -y build-essential wget

wget https://curl.se/download/curl-${CURL_VERSION}.tar.gz
tar -xzf curl-${CURL_VERSION}.tar.gz
cd curl-${CURL_VERSION}
./configure --without-ssl
make
make install
ldconfig

cd ..
gcc server.c -o server
gcc client.c -lcurl -o client

