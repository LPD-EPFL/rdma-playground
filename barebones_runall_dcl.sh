#!/bin/bash

set -o xtrace

# argument is 4 8 16 32 64 etc

homedir=$HOME
projdir=$homedir/RDMA-example
measurementsdir=$projdir/measurements
machines_dir='3m'

i=$1
ssh lpdquatro1 "cd $projdir && SZ=$1 ./deploy-dcl.sh start"
sleep 30
pid=`ssh lpdquatro1 tmux capture-pane -pt "peregrin" | grep "My pid is" | awk '{split($0,a,"My pid is "); print a[2]}'`
cd $measurementsdir/$machines_dir
ssh lpdquatro1 "kill -10 $pid"
sleep 5
mv $projdir/peregrin_deployment/dump-*.txt dump-$i.txt
sleep 5
ssh lpdquatro1 "cd $projdir && ./deploy-dcl.sh stop"