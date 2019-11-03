#!/bin/bash

set -o xtrace

# argument is 4 8 16 32 64 etc

homedir=$HOME
projdir=$homedir/RDMA-example
measurementsdir=$projdir/measurements/rereg-huge

readable_sizes=( "4MB" "16MB" "64MB" "256MB" "1GB" "4GB" )
i=0

for size in '(4ul*1024*1024)' '(16ul*1024*1024)' '(64ul*1024*1024)' '(256ul*1024*1024)' '(1ul*1024*1024*1024)' '(4ul*1024*1024*1024)'
do
    readable_size=${readable_sizes[$i]}
    echo $readable_size
    i=$((i+1))
    sed -i "s/#define DEFAULT_LOG_LENGTH .*/#define DEFAULT_LOG_LENGTH $size/" $projdir/include/log.h
    ninja -C $projdir/builddir install
    ssh lpdquatro1 "cd $projdir && ./deploy-dcl.sh start"
    sleep 15
    pid=`ssh lpdquatro1 tmux capture-pane -pt "peregrin" | grep "My pid is" | awk '{split($0,a,"My pid is "); print a[2]}'`
    cd $measurementsdir/$machines_dir
    ssh lpdquatro1 "kill -10 $pid"
    sleep 5
    mv $projdir/peregrin_deployment/dump-*.txt dump-$readable_size.txt
    sleep 5
    ssh lpdquatro1 "cd $projdir && ./deploy-dcl.sh stop"
done
