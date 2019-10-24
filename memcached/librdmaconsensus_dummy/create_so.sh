#!/bin/bash

gcc -Wall -Werror -fpic -shared -o librdmaconsensus.so main.c
