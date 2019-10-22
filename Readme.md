### Installation instructions

Install libraries to ~/.local
```sh
$ mkdir ~/.local
```

Add the following to your ~/.bashrc and reload the config:
```
export PATH=~/.local/bin:$PATH
export LD_LIBRARY_PATH=~/.local/lib:$LD_LIBRARY_PATH
export LIBRARY_PATH=~/.local/lib:$LIBRARY_PATH
export C_INCLUDE_PATH=~/.local/include:$C_INCLUDE_PATH
export CPLUS_INCLUDE_PATH=~/.local/include:$CPLUS_INCLUDE_PATH
```
```sh
$ source ~/.bashrc
```

Install meson:
```sh
$ pip3 install --user meson
```

Install required libraries. In particular, gtest is missing
```sh
$ git clone https://github.com/google/googletest.git
$ cd googletest/
$ git checkout tags/v1.10.x
$ mkdir build && cd build
$ cmake -DCMAKE_INSTALL_PREFIX=~/.local ..
$ make
$ make install
```

### Compilation
```sh
$ cd rdma-playground/
$ meson builddir && cd builddir
$ ninja
```

In order to build the project with libraries installed locally, use the following command insted:
```sh
$ meson --libdir=lib --prefix=~/.local builddir
```

Instead of `ninja` you can compile only the `propose-test` target as follows:
```sh
$ ninja propose-test
```

Finally, run the binary:
```sh
$ CONFIG=../config/config.0.toml ./propose-test
```

## Reproducible build
git clone https://github.com/LPD-EPFL/rdma-playground
cd rdma-playground/
git checkout redis_with_pump

meson --libdir=lib --prefix=~/.local builddir
cd builddir/
ninja
ninja install

cd ..

cd redis/
tar xf redis-5.0.5.tar.gz
patch -p1 -d redis-5.0.5 < patch.diff
cd redis-5.0.5/
make -j




Edit the config files
Start memcached on the the machine defined in memcached.


CONFIG=config/config.0.toml IS_LEADER=1 redis/redis-5.0.5/src/redis-server redis/redis.conf
CONFIG=config/config.1.toml redis/redis-5.0.5/src/redis-server redis/redis.conf
CONFIG=config/config.2.toml redis/redis-5.0.5/src/redis-server redis/redis.conf
Note: If you want to use uds, instead of tcp, you have to edit the redis patch.

Test with python (run on leader):
pip3 install --user redis

#!/usr/bin/env python3
import redis, time

r = redis.Redis()
for i in range(10**6):
	r.set("{:016d}".format(i), "value")
	time.sleep(1)

On the follower do:
#!/usr/bin/env python3
import redis, time

r = redis.Redis()
x = []
for i in range(10**6):
    x.append(r.get("{:016d}".format(i)))

assert len(x) == 10**6
