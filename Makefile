CC=gcc
CXX=g++
CFLAGS=-Wall -g
CXXFLAGS=-Wall -Wno-unused-function -g -I$(GTEST_INCLUDE) 
LDFLAGS=-L$(GTEST_LIB) -lgtest -lrdmacm -libverbs -pthread
BIN=./bin/main

# SOURCES    := ${wildcard *.c}
# OBJECTS    := ${SOURCES:.c=.o}

.PHONY:    all clean

.SUFFIXES: .c .o

all: tests

# main:      $(OBJECTS)

# ibv_layer:
# 	$(CC) $(CFLAGS) ibv_layer.c -o ibv_layer.o $(LDFLAGS)

rdma-consensus: 		
	$(CC) $(CFLAGS) utils.c ibv_layer.c  consensus-protocol.c rdma-consensus.c leader-election.c test.c -o $(BIN) $(LDFLAGS)

tests:
	$(CXX) $(CXXFLAGS) utils.c ibv_layer.c  consensus-protocol.c rdma-consensus.c leader-election.c tests.cpp  -o ./bin/tests $(LDFLAGS)
# 	$(CXX) $(CXXFLAGS) tests.cpp  -o ./bin/tests $(LDFLAGS)



clean:
	-rm -fv *.o
	-rm -fv ./bin/*