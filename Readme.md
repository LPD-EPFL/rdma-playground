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
