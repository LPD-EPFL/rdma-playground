#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# nodes=("h193" "h194" "h195")
# nodes=("h193" "h194" "h195" "h199" "h200")
# nodes=("h193" "h194" "h195" "h199" "h200" "h202" "h203")
nodes=("h193" "h194" "h195" "h199" "h200" "h202" "h203" "h210" "h211")

startme() {
    # Start memcached on the first node
    # ip=`getent hosts ${nodes[0]} | awk '{ print $1 }'`

    for ((j=0; j<${#nodes[@]}; j++)); do
        # if [[ "x${nodes[$j]}" == "x$HOSTNAME" ]]; then
        #     continue
        # fi

        if [ "$j" -eq "0" ]; then
            start_memcached='tmux new-window -n memcached -t peregrin:1; tmux send-keys -t peregrin:memcached "memcached -l 0.0.0.0 -vv" C-m; tmux select-window -t peregrin:0'
            LEADER="IS_LEADER=1"
        else
            start_memcached=""
            LEADER=""
        fi

ssh "$USER@${nodes[$j]}" /bin/bash << EOF
    tmux start-server
    pkill -9 memcached
    pkill -9 redis-server
    tmux kill-session -t peregrin 2> /dev/null
    mkdir -p $DIR/peregrin_deployment
    cd $DIR/peregrin_deployment
    tmux new-session -d -n main -s peregrin
    $start_memcached
    tmux send-keys -t peregrin:main "CONFIG=../config/helvetios/${#nodes[@]}/config.${nodes[$j]}.toml $LEADER ../redis/redis-2.8.17/src/redis-server ../../redis.conf" C-m
EOF

    done
    echo "Done"
}

stopme() {
    for ((j=0; j<${#nodes[@]}; j++)); do
        ssh "$USER@${nodes[$j]}" "pkill -9 memcached; killall memcached; pkill -9 redis-server; killall redis-server; tmux kill-session -t peregrin"
    done
}

case "$1" in
    start)   startme ;;
    stop)    stopme ;;
    *) echo "usage: $0 start|stop" >&2
       exit 1
       ;;
esac
