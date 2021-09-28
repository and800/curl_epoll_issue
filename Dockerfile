FROM ubuntu:21.04
STOPSIGNAL SIGKILL
CMD sleep infinity
RUN apt-get update && apt-get install -y build-essential wget htop && mkdir /work
WORKDIR /work
COPY client.c server.c test.sh /work/
