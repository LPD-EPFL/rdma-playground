CC=gcc
CFLAGS=-Wall -g
LDFLAGS= -lrdmacm -libverbs -pthread
BIN=./bin/main

# SOURCES    := ${wildcard *.c}
# OBJECTS    := ${SOURCES:.c=.o}

.PHONY:    all clean

.SUFFIXES: .c .o

all: rdma-consensus

# main:      $(OBJECTS)

# ibv_layer:
# 	$(CC) $(CFLAGS) ibv_layer.c -o ibv_layer.o $(LDFLAGS)

rdma-consensus: 
	$(CC) $(CFLAGS) rdma-consensus.c ibv_layer.c -o $(BIN) $(LDFLAGS)
 
# .c.o:
# 	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)



clean:
	-rm -fv *.o