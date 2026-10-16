# Host tests for ReliableSerialComms: the library against a loopback model of the platform layer.
# make                      - build and run the default and the RSC_NO_LEGACY build, and check the sources as C++
# make CXX=clang++ STD=c++20
# The C sources are built with gcc, or with clang when CXX is clang++.

CXX  ?= g++
STD  ?= gnu++17
CC   := $(if $(findstring clang,$(CXX)),clang,gcc)
CSTD ?= gnu99

SAN      = -fsanitize=address,undefined -fno-sanitize-recover=all
COMMON   = -g -O1 -Wall -Wextra -Werror $(SAN) -I ../../src -I stubs
CFLAGS   = -std=$(CSTD) $(COMMON)
CXXFLAGS = -std=$(STD) $(COMMON) -DRSC_REPO_DIR=\"../..\"
SRC      = ../../src/rsc_framework.c ../../src/rsc_crc.c
TSRC     = test_rsc.cpp stubs/platform_model.cpp
DEPS     = $(SRC) $(TSRC) $(wildcard ../../src/*.h) $(wildcard stubs/*.h) Makefile

all: run

test_rsc: $(DEPS)
	$(CC) $(CFLAGS) -c ../../src/rsc_framework.c -o rsc_framework.o
	$(CC) $(CFLAGS) -c ../../src/rsc_crc.c -o rsc_crc.o
	$(CXX) $(CXXFLAGS) -o $@ $(TSRC) rsc_framework.o rsc_crc.o

test_rsc_nolegacy: $(DEPS)
	$(CC) $(CFLAGS) -DRSC_NO_LEGACY -c ../../src/rsc_framework.c -o rsc_framework_nolegacy.o
	$(CC) $(CFLAGS) -DRSC_NO_LEGACY -c ../../src/rsc_crc.c -o rsc_crc_nolegacy.o
	$(CXX) $(CXXFLAGS) -DRSC_NO_LEGACY -o $@ $(TSRC) rsc_framework_nolegacy.o rsc_crc_nolegacy.o

cxx_check: cxx_check.cpp $(DEPS)
	$(CXX) -std=$(STD) -Wall -Wextra -Werror -I ../../src -fsyntax-only cxx_check.cpp

run: test_rsc test_rsc_nolegacy cxx_check
	./test_rsc
	./test_rsc_nolegacy

clean:
	rm -f test_rsc test_rsc_nolegacy *.o

.PHONY: all run clean cxx_check
