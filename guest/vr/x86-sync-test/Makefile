# steamac guest VR: OpenVR runtime (vrclient.so), vr-fakehost, vr-testapp, unit tests. x86-64 Linux.
#   make              build everything into $(OUT)
#   make check        unit tests + end-to-end test (fake host + test app), native
#   make stubs        regenerate runtime/stubs.gen.h from third_party/openvr/openvr.h
# Release builds use the Steam Runtime sniper SDK (glibc 2.31, what Proton runs unixlibs in): build-sniper.sh.
OUT ?= build
CXX ?= g++
CC ?= gcc
CPPFLAGS = -Iinclude -Iruntime -Ithird_party/openvr
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -fPIC
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra

RUNTIME_SRC = runtime/clientcore.cpp runtime/system.cpp runtime/compositor.cpp runtime/input.cpp \
              runtime/input_bindings.cpp runtime/misc.cpp runtime/runtime.cpp runtime/transport.cpp \
              runtime/json.cpp runtime/log.cpp
HDRS = $(wildcard include/*.h runtime/*.h)

all: $(OUT)/runtime/bin/linux64/vrclient.so $(OUT)/vr-fakehost $(OUT)/vr-testapp $(OUT)/unit_tests

# Proton loads <runtime>/bin/linux64/vrclient.so (PROTON_VR_RUNTIME=<runtime>)
$(OUT)/runtime/bin/linux64/vrclient.so: $(RUNTIME_SRC) $(HDRS)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fvisibility=hidden -shared -o $@ $(RUNTIME_SRC) \
	  -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,-z,defs -lpthread -ldl

$(OUT)/vr-fakehost: fakehost/vr_fakehost.c include/steamac_vr_guest.h
	mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< -lm

$(OUT)/vr-testapp: tests/vr_testapp.cpp include/steamac_vr_guest.h
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< -ldl -lm

$(OUT)/unit_tests: tests/unit_tests.cpp runtime/input_bindings.cpp runtime/json.cpp $(HDRS)
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/unit_tests.cpp runtime/input_bindings.cpp runtime/json.cpp -lm

check: all
	$(OUT)/unit_tests
	tests/run_e2e.sh $(OUT)

stubs:
	python3 tools/gen_stubs.py third_party/openvr/openvr.h > runtime/stubs.gen.h

clean:
	rm -rf $(OUT)

.PHONY: all check stubs clean
