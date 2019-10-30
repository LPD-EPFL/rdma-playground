#!/bin/bash

bindir=$(pwd)'/bin-1.2.6'
memdir=$(pwd)'/memcached-1.2.6/'

if [ ! -d "$bindir" ]
then
	mkdir $bindir
fi

if [ ! -f "1.2.6.tar.gz" ]
then
    wget https://github.com/memcached/memcached/archive/1.2.6.tar.gz
fi
tar -xf 1.2.6.tar.gz
patch -p1 -d $memdir <patch1.2.6.diff
cd $memdir && sh ./autogen.sh
cd $memdir && sh ./configure --prefix=$bindir
sed -i "s/LIBS =  -levent/LIBS =  -levent\nLIBS += -libverbs -lrdmaconsensus -lnuma -pthread -lmemcached/" $memdir/Makefile
cd $memdir && make
cd $memdir && make install
