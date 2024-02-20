CC=gcc
CFLAGS=-g -pedantic -std=gnu17 -Wall -Werror -Wextra
LDFLAGS=-pthread

.PHONY: all
all: rle

rle: rle.o

rle.o: rle.c

.PHONY: clean
clean:
	rm -f *.o rle
