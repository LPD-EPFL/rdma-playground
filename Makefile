CC=gcc
CXX=g++
CFLAGS=-Wall -g
CXXFLAGS=-O3 -Wall -Wno-unused-function -g -D_GNU_SOURCE
LDFLAGS=-lmemcached -lgtest -lrdmacm -libverbs -pthread
BIN=./bin/main

# SOURCES    := ${wildcard *.c}
# OBJECTS    := ${SOURCES:.c=.o}

.PHONY:    all clean

.SUFFIXES: .c .o

all: tests

# main:      $(OBJECTS)

# ibv_layer:
# 	$(CC) $(CFLAGS) ibv_layer.c -o ibv_layer.o $(LDFLAGS)

# Common object files

rdma-consensus:
	$(CC) $(CFLAGS) utils.c ibv_layer.c  consensus-protocol.c rdma-consensus.c leader-election.c test.c -o $(BIN) $(LDFLAGS)

tests:
	$(CXX) $(CXXFLAGS) utils.c ibv_layer.c  consensus-protocol.c rdma-consensus.c leader-election.c tests.cpp  -o ./bin/tests $(LDFLAGS)
# 	$(CXX) $(CXXFLAGS) tests.cpp  -o ./bin/tests $(LDFLAGS)

propose-test:
	$(CC) $(CXXFLAGS) utils.c parser.c registry.c toml/toml.c ibv_layer.c  consensus-protocol.c rdma-consensus.c leader-election.c propose_api.c propose_main.c   -o ./bin/propose_main $(LDFLAGS)


%.o: %.c
	$(CC) $(CXXFLAGS) $(LDFLAGS) $< -c

propose-test-obj: utils.o ibv_layer.o consensus-protocol.o rdma-consensus.o leader-election.o propose_api.o propose_main.o
	cp $^ obj
	$(CC) $(CXXFLAGS) $^ -o ./bin/propose_main $(LDFLAGS)



clean:
	-rm -fv *.o
	-rm -fv obj/*.o
	-rm -fv ./bin/*
