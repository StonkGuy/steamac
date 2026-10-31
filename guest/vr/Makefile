# steamac guest VR: OpenVR runtime (vrclient.so), vr-fakehost, vr-testapp, unit tests. x86-64 Linux.
#   make              build everything into $(OUT)
#   make check        unit tests + end-to-end test (fake host + test app), native
#   make stubs        regenerate runtime/stubs.gen.h from third_party/openvr/openvr.h
#   make legacy       regenerate runtime/legacy.gen.h (older interface versions) from
#                     third_party/openvr/legacy_interfaces.txt and runtime/legacy_overrides.h
#   make check-legacy OPENVR_GIT=<ValveSoftware/openvr clone>   layout of every older version against its header
# Release builds use the Steam Runtime sniper SDK (glibc 2.31, what Proton runs unixlibs in): build-sniper.sh.
OUT ?= build
CXX ?= g++
CC ?= gcc
CPPFLAGS = -Iinclude -Iruntime -Ithird_party/openvr
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -fPIC
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers
# the runtime is loaded into Wine processes inside the Steam Runtime: carry its own libstdc++
RUNTIME_LDFLAGS ?= -static-libstdc++ -static-libgcc

RUNTIME_SRC = runtime/clientcore.cpp runtime/system.cpp runtime/compositor.cpp runtime/input.cpp \
              runtime/input_bindings.cpp runtime/misc.cpp runtime/runtime.cpp runtime/transport.cpp \
              runtime/json.cpp runtime/log.cpp runtime/overlay.cpp runtime/skeleton.cpp
HDRS = $(wildcard include/*.h runtime/*.h)

all: $(OUT)/runtime/bin/linux64/vrclient.so $(OUT)/vr-fakehost $(OUT)/vr-testapp $(OUT)/unit_tests $(OUT)/transport_test

# Proton loads <runtime>/bin/linux64/vrclient.so (PROTON_VR_RUNTIME=<runtime>)
$(OUT)/runtime/bin/linux64/vrclient.so: $(RUNTIME_SRC) $(HDRS)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fvisibility=hidden -shared -o $@ $(RUNTIME_SRC) \
	  $(RUNTIME_LDFLAGS) -Wl,--no-undefined -Wl,-z,defs -lpthread -ldl

$(OUT)/vr-fakehost: fakehost/vr_fakehost.c include/steamac_vr_guest.h
	mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< -lm

$(OUT)/vr-testapp: tests/vr_testapp.cpp include/steamac_vr_guest.h runtime/legacy.gen.h
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< -ldl -lm

UNIT_SRC = tests/unit_tests.cpp runtime/input_bindings.cpp runtime/json.cpp runtime/skeleton.cpp \
           runtime/overlay.cpp runtime/log.cpp
$(OUT)/unit_tests: $(UNIT_SRC) $(HDRS)
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(UNIT_SRC) -lm

# Transport::open_wmtr without DXMT loaded: loads winemetal.so beside Wine's ntdll.so (fakes in transport/)
$(OUT)/transport_test: tests/transport_test.cpp tests/fake_winemetal.c runtime/transport.cpp runtime/log.cpp $(HDRS)
	mkdir -p $(OUT)/transport/x86_64-unix
	echo 'int fake_ntdll;' | $(CC) -x c -shared -fPIC -o $(OUT)/transport/x86_64-unix/ntdll.so -
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $(OUT)/transport/x86_64-unix/winemetal.so tests/fake_winemetal.c
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/transport_test.cpp runtime/transport.cpp runtime/log.cpp -ldl -lpthread

check: all
	$(OUT)/unit_tests
	$(OUT)/transport_test $(OUT)/transport/x86_64-unix
	tests/check_legacy.sh $(OUT)
	tests/run_e2e.sh $(OUT)

# every older interface version against the header of the OpenVR release it shipped in
check-legacy:
	tests/check_legacy.sh $(OUT) $(OPENVR_GIT)

stubs:
	python3 tools/gen_stubs.py third_party/openvr/openvr.h > runtime/stubs.gen.h

legacy:
	python3 tools/gen_legacy.py gen third_party/openvr/legacy_interfaces.txt third_party/openvr/openvr.h > runtime/legacy.gen.h

clean:
	rm -rf $(OUT)

.PHONY: all check check-legacy stubs legacy clean
