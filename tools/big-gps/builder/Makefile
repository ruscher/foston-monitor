# Big GPS map builder (PC).  Needs libosmium and protozero headers (see docs/big-gps/03-arquitetura.md).
# SPDX-License-Identifier: GPL-3.0-or-later
DEPS ?= ../../../firmware/build
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -I$(DEPS)/src-libosmium/include -I$(DEPS)/src-protozero/include
LDLIBS = -lz -lpthread

all: bgmap-build
bgmap-build: bgmap-build.cpp ../../../src/ui/biggps/bgmap_format.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)
clean:
	rm -f bgmap-build
.PHONY: all clean
