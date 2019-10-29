#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# It is advised to compile with gcc v4

cd $DIR
tar xf redis-2.8.17.tar.gz

patch -p1 -d redis-2.8.17 < patch-2.8.17-v2.diff

make -C redis-2.8.17 -j
