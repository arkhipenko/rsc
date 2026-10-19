# The C peer of the interop tests (python/tests/test_interop.py)
CC     ?= gcc
CFLAGS  = -std=c99 -O1 -g -Wall -Wextra -Werror -I ../../src
SRC     = c_peer.c ../../src/rsc.c ../../src/rsc_cobs.c ../../src/rsc_crc.c

c_peer: $(SRC) $(wildcard ../../src/*.h) Makefile
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f c_peer

.PHONY: clean
