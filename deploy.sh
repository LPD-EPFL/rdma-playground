#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

nodes=("h193" "h194" "h195")
#nodes=("h193" "h194" "h195" "h199" "h200")
#nodes=("h193" "h194" "h195" "h199" "h200" "h202" "h203")
#nodes=("h193" "h194" "h195" "h199" "h200" "h202" "h203" "h210" "h211")

config_toml=$(cat <<EOF
[registry]
    host = \"#host#\"
    port = 11211

[general]
    clients = #clients#
    id = #id#
EOF
)

startme() {
    # Start memcached on the first node
    # ip=`getent hosts ${nodes[0]} | awk '{ print $1 }'`
    ip='10.91.19.193'

    for ((j=0; j<${#nodes[@]}; j++)); do
        # if [[ "x${nodes[$j]}" == "x$HOSTNAME" ]]; then
        #     continue
        # fi

        if [ "$j" -eq "0" ]; then
            start_memcached='tmux new-window -n memcached -t peregrin:1; tmux send-keys -t peregrin:memcached "memcached -l 0.0.0.0" C-m; tmux select-window -t peregrin:0'
        else
            start_memcached=""
        fi

ssh "$USER@${nodes[$j]}" /bin/bash << EOF
    module load tmux
    tmux start-server
    tmux kill-session -t peregrin 2> /dev/null
    killall memcached
    mkdir -p $DIR/peregrin_deployment
    cd $DIR/peregrin_deployment
    echo "$config_toml" > config.${nodes[$j]}.toml
    tmux new-session -d -n main -s peregrin
    $start_memcached
    export PEREGRIN_NN=${nodes[$j]}
    sed -i "s/#host#/$ip/" config.${nodes[$j]}.toml
    sed -i "s/#clients#/${#nodes[@]}/" config.${nodes[$j]}.toml
    sed -i "s/#id#/$j/" config.${nodes[$j]}.toml
    tmux send-keys -t peregrin:main "CONFIG=config.${nodes[$j]}.toml $DIR/builddir/propose-test" C-m
EOF

    done
    echo "Done"
}

stopme() {
    for ((j=0; j<${#nodes[@]}; j++)); do
        ssh "$USER@${nodes[$j]}" "module load tmux; killall memcached; tmux kill-session -t peregrin"
    done
}

case "$1" in
    start)   startme ;;
    stop)    stopme ;;
    *) echo "usage: $0 start|stop" >&2
       exit 1
       ;;
esac
