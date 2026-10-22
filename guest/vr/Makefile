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
              runtime/json.cpp runtime/log.cpp runtime/overlay.cpp runtime/skeleton.cpp runtime/trackers.cpp
HDRS = $(wildcard include/*.h runtime/*.h)

all: $(OUT)/runtime/bin/linux64/vrclient.so $(OUT)/vr-fakehost $(OUT)/vr-testapp $(OUT)/unit_tests $(OUT)/transport_test \
     $(OUT)/svr-driverhost $(OUT)/svr-tracker-feed $(OUT)/vr-trackerapp $(OUT)/svrtest/bin/linux64/driver_svrtest.so

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
           runtime/overlay.cpp runtime/log.cpp runtime/trackers.cpp
$(OUT)/unit_tests: $(UNIT_SRC) $(HDRS)
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(UNIT_SRC) -lm

# Transport::open_wmtr without DXMT loaded: loads winemetal.so beside Wine's ntdll.so (fakes in transport/)
$(OUT)/transport_test: tests/transport_test.cpp tests/fake_winemetal.c runtime/transport.cpp runtime/log.cpp $(HDRS)
	mkdir -p $(OUT)/transport/x86_64-unix
	echo 'int fake_ntdll;' | $(CC) -x c -shared -fPIC -o $(OUT)/transport/x86_64-unix/ntdll.so -
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $(OUT)/transport/x86_64-unix/winemetal.so tests/fake_winemetal.c
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/transport_test.cpp runtime/transport.cpp runtime/log.cpp -ldl -lpthread

# tracked devices (docs/vr-trackers.md): driver host for SteamVR drivers, a reference tracker source, their tests.
# svr-driverhost.exe (Windows drivers under Wine/Proton): make driverhost-win MINGW=x86_64-w64-mingw32-
$(OUT)/svr-driverhost: driverhost/svr_driverhost.cpp runtime/trackers.cpp runtime/json.cpp $(HDRS)
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ driverhost/svr_driverhost.cpp runtime/trackers.cpp runtime/json.cpp \
	  $(RUNTIME_LDFLAGS) -ldl -lpthread

$(OUT)/svr-tracker-feed: tools/svr_tracker_feed.cpp runtime/trackers.cpp $(HDRS)
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tools/svr_tracker_feed.cpp runtime/trackers.cpp $(RUNTIME_LDFLAGS) -lm

$(OUT)/vr-trackerapp: tests/vr_trackerapp.cpp
	mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< -ldl -lm

$(OUT)/svrtest/bin/linux64/driver_svrtest.so: tests/sample_driver/driver_svrtest.cpp
	mkdir -p $(dir $@)
	cp -r tests/sample_driver/svrtest/. $(OUT)/svrtest/
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fvisibility=hidden -shared -o $@ $< -lm

MINGW ?= x86_64-w64-mingw32-
driverhost-win: $(OUT)/win/svr-driverhost.exe $(OUT)/win/svrtest/bin/win64/driver_svrtest.dll $(OUT)/win/svrtest/companion.exe
$(OUT)/win/svr-driverhost.exe: driverhost/svr_driverhost.cpp runtime/trackers.cpp runtime/json.cpp $(HDRS)
	mkdir -p $(dir $@)
	$(MINGW)g++ $(CPPFLAGS) -O2 -std=c++17 -o $@ driverhost/svr_driverhost.cpp runtime/trackers.cpp runtime/json.cpp \
	  -static -static-libgcc -static-libstdc++
$(OUT)/win/svrtest/bin/win64/driver_svrtest.dll: tests/sample_driver/driver_svrtest.cpp
	mkdir -p $(dir $@)
	cp -r tests/sample_driver/svrtest/. $(OUT)/win/svrtest/
	$(MINGW)g++ $(CPPFLAGS) -O2 -std=c++17 -shared -o $@ $< -static -static-libgcc -static-libstdc++
$(OUT)/win/svrtest/companion.exe: tests/sample_driver/companion.c
	mkdir -p $(dir $@)
	$(MINGW)gcc -O2 -o $@ $< -static
# under Wine (wine64 in PATH): the driver host starts the companion, which talks to its driver over a named pipe
check-wine: driverhost-win $(OUT)/svr-tracker-feed
	tests/run_trackers_wine.sh $(OUT)

check: all
	$(OUT)/unit_tests
	$(OUT)/transport_test $(OUT)/transport/x86_64-unix
	tests/check_legacy.sh $(OUT)
	tests/run_e2e.sh $(OUT)
	tests/run_trackers.sh $(OUT)

# every older interface version against the header of the OpenVR release it shipped in
check-legacy:
	tests/check_legacy.sh $(OUT) $(OPENVR_GIT)

stubs:
	python3 tools/gen_stubs.py third_party/openvr/openvr.h > runtime/stubs.gen.h

legacy:
	python3 tools/gen_legacy.py gen third_party/openvr/legacy_interfaces.txt third_party/openvr/openvr.h > runtime/legacy.gen.h

clean:
	rm -rf $(OUT)

.PHONY: all check driverhost-win check-wine check-legacy stubs legacy clean
