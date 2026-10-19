# Host tests for ReliableSerialComms: two links joined by a simulated wire with faults.
# make                      - build and run three configurations, and check the sources as C++
# make CXX=clang++ STD=c++20
# The C sources are built as C99 with gcc, or with clang when CXX is clang++.
#
# Builds:
#   test_rsc          defaults (MTU 256, 8 slots, window 4, byte-table CRC)
#   test_rsc_small    the AVR defaults (MTU 64, 2 slots, window 1, 2 requests, no counters, nibble CRC)
#   test_rsc_wide     16 slots, window 7, bitwise CRC

CXX  ?= g++
STD  ?= gnu++17
CC   := $(if $(findstring clang,$(CXX)),clang,gcc)
CSTD ?= c99

SAN      = -fsanitize=address,undefined -fno-sanitize-recover=all
COMMON   = -g -O1 -Wall -Wextra -Werror $(SAN) -I ../../src
CFLAGS   = -std=$(CSTD) -pedantic $(COMMON)
CXXFLAGS = -std=$(STD) $(COMMON)
SRC      = ../../src/rsc.c ../../src/rsc_cobs.c ../../src/rsc_crc.c
DEPS     = $(SRC) test_rsc.cpp $(wildcard ../../src/*.h) Makefile

SMALL    = -DRSC_MTU=64 -DRSC_TX_SLOTS=2 -DRSC_WINDOW=1 -DRSC_MAX_REQUESTS=2 -DRSC_STATS=0 -DRSC_CRC_IMPL=1
WIDE     = -DRSC_TX_SLOTS=16 -DRSC_WINDOW=7 -DRSC_CRC_IMPL=0

all: run

# $(1) target, $(2) defines
define build
$(1): $(DEPS)
	$(CC) $(CFLAGS) $(2) -c ../../src/rsc.c -o $(1)_rsc.o
	$(CC) $(CFLAGS) $(2) -c ../../src/rsc_cobs.c -o $(1)_cobs.o
	$(CC) $(CFLAGS) $(2) -c ../../src/rsc_crc.c -o $(1)_crc.o
	$(CXX) $(CXXFLAGS) $(2) -o $(1) test_rsc.cpp $(1)_rsc.o $(1)_cobs.o $(1)_crc.o
endef

$(eval $(call build,test_rsc,))
$(eval $(call build,test_rsc_small,$(SMALL)))
$(eval $(call build,test_rsc_wide,$(WIDE)))

cxx_check: cxx_check.cpp $(DEPS)
	$(CXX) -std=$(STD) -Wall -Wextra -Werror -I ../../src -fsyntax-only cxx_check.cpp

run: test_rsc test_rsc_small test_rsc_wide cxx_check
	./test_rsc
	./test_rsc_small
	./test_rsc_wide

clean:
	rm -f test_rsc test_rsc_small test_rsc_wide *.o

.PHONY: all run clean cxx_check
