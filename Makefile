CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
LDFLAGS ?=

all: bserve bcurl

bserve: src/bserve.o src/octet.o
	$(CC) $(LDFLAGS) -o $@ $^

bcurl: src/bcurl.o src/octet.o
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c src/octet.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: all
	./tests/run.sh

clean:
	rm -f bserve bcurl src/*.o
	rm -rf tests/tmp

.PHONY: all test clean
