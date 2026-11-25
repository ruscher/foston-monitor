# Big GPS map and route builders (PC).  Need libosmium, protozero and RoutingKit (docs/big-gps/05-construtor-mapas.md).
# SPDX-License-Identifier: GPL-3.0-or-later
DEPS ?= ../../../firmware/build
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -I$(DEPS)/src-libosmium/include -I$(DEPS)/src-protozero/include
LDLIBS = -lz -lpthread

RK = $(DEPS)/src-routingkit

all: bgmap-build bgmap-merge bgroute-build bgsearch-build bgalert-build
bgmap-build: bgmap-build.cpp ../../../src/ui/biggps/bgmap_format.h ../../../src/ui/biggps/bgkinds.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)
bgmap-merge: bgmap-merge.cpp ../../../src/ui/biggps/bgmap_format.h
	$(CXX) $(CXXFLAGS) -o $@ $<
bgalert-build: bgalert-build.cpp ../../../src/ui/biggps/bgalert_format.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)
bgsearch-build: bgsearch-build.cpp ../../../src/ui/biggps/bgsearch_format.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)
# RoutingKit (BSD-2-Clause): make -C $(RK) lib/libroutingkit.a
bgroute-build: bgroute-build.cpp ../../../src/ui/biggps/bgroute_format.h $(RK)/lib/libroutingkit.a
	$(CXX) $(CXXFLAGS) -I$(RK)/include -fopenmp -o $@ $< $(RK)/lib/libroutingkit.a $(LDLIBS)
$(RK)/lib/libroutingkit.a:
	$(MAKE) -C $(RK) lib/libroutingkit.a
clean:
	rm -f bgmap-build bgmap-merge bgroute-build bgsearch-build bgalert-build
.PHONY: all clean
