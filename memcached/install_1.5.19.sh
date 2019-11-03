#!/bin/bash

set -x

bindir=$(pwd)'/bin-1.5.19'
memdir=$(pwd)'/memcached-1.5.19/'

if [ ! -d "$bindir" ]
then
	mkdir $bindir
fi

if [ ! -f "1.5.19.tar.gz" ]
then
    wget https://github.com/memcached/memcached/archive/1.5.19.tar.gz
fi
tar -xf 1.5.19.tar.gz
patch -p1 -d $memdir <patch1.5.19.diff
cd $memdir && sh ./configure --prefix=$bindir
sed -i "s/LIBS = -lhugetlbfs -levent/LIBS = -lhugetlbfs -levent\nLIBS += -libverbs -lrdmaconsensus -lnuma -pthread -lmemcached/" $memdir/Makefile
cd $memdir && make
cd $memdir && make install
