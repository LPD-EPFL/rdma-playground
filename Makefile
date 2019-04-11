CC=gcc
CFLAGS=-Wall -g
LDFLAGS= -lrdmacm -libverbs

SOURCES    := ${wildcard *.c}
OBJECTS    := ${SOURCES:.c=.o}

.PHONY:    all clean

.SUFFIXES: .c .o

all:       main

main:      $(OBJECTS)

.c.o:
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	-rm -fv *.o