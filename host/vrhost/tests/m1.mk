# SPDX-License-Identifier: GPL-3.0-or-later
# M1 compositor + link tests, buildable without the rest of steamac-vrhost (CMake target: see compositor.cmake).
#   make -f tests/m1.mk test DXMT=<StonkGuy/dxmt vr-export checkout>
DXMT ?= ../../../dxmt
OUT ?= build-m1
CXX = clang++
FLAGS = -O2 -g -Wall -Wno-unused-parameter -fobjc-arc -Isrc -Iinclude -I../../wmtr/include -I../../guest/vr/include -I$(DXMT)/src/winemetal
FW = -framework Foundation -framework Metal -framework IOSurface -framework CoreVideo
SRC = src/compositor/foveation_params.cpp src/compositor/eye_compositor.mm src/compositor/vrlink_server.mm

$(OUT)/m1_pipeline_test: tests/m1_pipeline_test.mm $(SRC) src/compositor/*.h include/vrlink.h ../../wmtr/src/wmtr_vr.m
	mkdir -p $(OUT)
	clang -O2 -g -Wall -ObjC -Iinclude -I../../wmtr/include -I../../guest/vr/include -I$(DXMT)/src/winemetal -c ../../wmtr/src/wmtr_vr.m -o $(OUT)/wmtr_vr.o
	$(CXX) -std=c++20 $(FLAGS) -x objective-c++ -o $@ tests/m1_pipeline_test.mm src/compositor/eye_compositor.mm \
	  src/compositor/vrlink_server.mm -x c++ src/compositor/foveation_params.cpp -x none $(OUT)/wmtr_vr.o $(FW)

test: $(OUT)/m1_pipeline_test
	./$(OUT)/m1_pipeline_test

.PHONY: test
