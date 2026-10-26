# SPDX-License-Identifier: GPL-3.0-or-later
# steamac-vrhost: Mac-native VR host (WiVRn-compatible server). A separate executable from steamac-vm because it
# contains GPL-3.0-or-later WiVRn code; see COPYING and docs/vr.md.
cmake_minimum_required(VERSION 3.25)
project(steamac-vrhost LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(VRHOST_FUZZ)
	# -fsanitize=function breaks C++ exception handling on macOS (uncaught throws) with LLVM 23; alignment, bool and
	# enum: WiVRn's deserializer reads unaligned u32 and raw bool/enum bytes by design (benign on arm64/clang)
	add_compile_options(-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize=function,alignment,bool,enum -fno-omit-frame-pointer)
	# lld: Apple ld rejects some LLVM 23 objects ("invalid r_symbolnum")
	add_link_options(-fsanitize=address,undefined -fno-sanitize=function -fuse-ld=lld)
	# LLVM's libc++ does not pull <array> in transitively where Apple's does (third_party crypto.cpp)
	add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-includearray>)
endif()

if(NOT APPLE)
	message(FATAL_ERROR "steamac-vrhost is macOS only")
endif()

include(FetchContent)
# Header-only dependencies, same versions as WiVRn v26.9 (CMakeLists.txt there). The protocol hash depends on the
# OpenXR header's array sizes, so the OpenXR-SDK version must match the pinned WiVRn tag.
FetchContent_Declare(openxr_sdk URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.58.tar.gz
	SOURCE_SUBDIR do-not-add)
FetchContent_Declare(boost_pfr URL https://github.com/boostorg/pfr/archive/refs/tags/boost-1.89.0.tar.gz
	SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(openxr_sdk boost_pfr)

# OpenSSL (libcrypto only: X448, PBKDF2, AES-128-CTR, BIGNUM for WiVRn's crypto/smp/secrets). Default: a static
# libcrypto built from a pinned release tarball into the build directory, so steamac-vrhost links no Homebrew dylib.
# -DVRHOST_SYSTEM_OPENSSL=ON uses find_package(OpenSSL) instead (development only).
option(VRHOST_SYSTEM_OPENSSL "Link the system/Homebrew OpenSSL instead of the pinned static build" OFF)
if(VRHOST_SYSTEM_OPENSSL)
	find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
else()
	include(ExternalProject)
	set(VRHOST_OPENSSL_VERSION 3.5.4)
	set(VRHOST_OPENSSL_SHA256 967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99)
	set(_ossl_prefix ${CMAKE_BINARY_DIR}/openssl-static)
	if(CMAKE_OSX_DEPLOYMENT_TARGET)
		set(_ossl_min -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
	endif()
	ExternalProject_Add(openssl_static
		URL https://github.com/openssl/openssl/releases/download/openssl-${VRHOST_OPENSSL_VERSION}/openssl-${VRHOST_OPENSSL_VERSION}.tar.gz
		URL_HASH SHA256=${VRHOST_OPENSSL_SHA256}
		DOWNLOAD_EXTRACT_TIMESTAMP ON
		BUILD_IN_SOURCE ON
		CONFIGURE_COMMAND ./Configure darwin64-arm64-cc no-shared no-module no-dso no-engine no-tests no-apps no-docs
			no-ssl no-tls no-dtls no-quic no-ui-console no-autoload-config ${_ossl_min}
			--prefix=${_ossl_prefix} --openssldir=${_ossl_prefix}/ssl --libdir=lib
		BUILD_COMMAND make -j8 build_libs
		INSTALL_COMMAND make install_dev
		BUILD_BYPRODUCTS ${_ossl_prefix}/lib/libcrypto.a
		LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
	file(MAKE_DIRECTORY ${_ossl_prefix}/include)
	add_library(OpenSSL::Crypto STATIC IMPORTED)
	set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION ${_ossl_prefix}/lib/libcrypto.a
		INTERFACE_INCLUDE_DIRECTORIES ${_ossl_prefix}/include)
	add_dependencies(OpenSSL::Crypto openssl_static)
endif()

# --- WiVRn common (vendored unmodified from WiVRn v26.9, see third_party/wivrn/UPSTREAM.md) ---
add_library(wivrn-common STATIC
	third_party/wivrn/common/crypto.cpp
	third_party/wivrn/common/secrets.cpp
	third_party/wivrn/common/smp.cpp
	third_party/wivrn/common/wivrn_sockets.cpp)
target_include_directories(wivrn-common SYSTEM PUBLIC
	third_party/wivrn/common
	third_party/wivrn/external
	${openxr_sdk_SOURCE_DIR}/include
	${boost_pfr_SOURCE_DIR}/include)
target_compile_options(wivrn-common PUBLIC
	"SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/wivrn_macos_compat.h")
target_compile_options(wivrn-common PRIVATE -Wno-c++11-narrowing)
target_link_libraries(wivrn-common PUBLIC OpenSSL::Crypto)

# --- WiVRn pacer (server/compositor/pacer.*, unmodified) with minimal Monado stand-ins in compat/monado ---
add_library(wivrn-pacer STATIC
	third_party/wivrn/server/compositor/pacer.cpp
	src/wivrn/clock_offset_compat.cpp)
target_include_directories(wivrn-pacer SYSTEM PUBLIC third_party/wivrn/server compat/monado)
target_link_libraries(wivrn-pacer PUBLIC wivrn-common)
# std::jthread / std::stop_token are still behind -fexperimental-library in Apple's libc++.
target_compile_options(wivrn-pacer PUBLIC $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fexperimental-library>)
target_link_options(wivrn-pacer PUBLIC -fexperimental-library)

# --- host core (session interface, WiVRn adapter, encoder, test pattern) ---
add_library(vrhost-core STATIC
	src/session/clock.cpp
	src/session/pose_history.cpp
	src/audio/pcm.cpp
	src/audio/audio_bridge.cpp
	src/wivrn/wivrn_session.cpp
	src/wivrn/known_keys.cpp
	src/wivrn/dnssd_publisher.cpp
	src/wivrn/annexb.cpp
	src/wivrn/input_map.cpp
	src/control/control_server.cpp
	src/encoder/vt_encoder.mm
	src/pattern/test_pattern.mm)
target_include_directories(vrhost-core PUBLIC src include)
target_link_libraries(vrhost-core PUBLIC wivrn-common wivrn-pacer
	"-framework CoreFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework VideoToolbox"
	"-framework IOSurface" "-framework Metal" "-framework Foundation")
target_compile_options(vrhost-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(src/encoder/vt_encoder.mm src/pattern/test_pattern.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)

# --- game frames (M1 compositor + VR link) and the pipeline into the session ---
include(compositor.cmake)
target_sources(vrhost-compositor PRIVATE src/pipeline/vr_pipeline.mm)
set_source_files_properties(src/pipeline/vr_pipeline.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
target_link_libraries(vrhost-compositor PUBLIC vrhost-core)

add_executable(steamac-vrhost src/main.cpp)
target_link_libraries(steamac-vrhost PRIVATE vrhost-core vrhost-compositor)
# Ad hoc signature with a stable identifier (macOS keys Local Network permission and TCC prompts on it).
add_custom_command(TARGET steamac-vrhost POST_BUILD
	COMMAND codesign --force --sign - --identifier com.steamac.vrhost $<TARGET_FILE:steamac-vrhost>
	VERBATIM)

# --- tests (ctest) ---
option(VRHOST_TESTS "Build the tests" ON)
if(VRHOST_TESTS)
enable_testing()
foreach(t protocol annexb pose_history encoder loopback pacer frames_in_flight adaptive_release crypto audio control)
	add_executable(test_${t} tests/test_${t}.cpp)
	target_link_libraries(test_${t} PRIVATE vrhost-core)
	add_test(NAME ${t} COMMAND test_${t})
endforeach()
target_sources(test_loopback PRIVATE tests/fake_headset.cpp)
target_sources(test_control PRIVATE tests/fake_headset.cpp tests/vt_decode.mm)
target_sources(test_encoder PRIVATE tests/vt_decode.mm)
target_sources(test_loopback PRIVATE tests/vt_decode.mm)
set_source_files_properties(tests/vt_decode.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
set_tests_properties(encoder loopback control PROPERTIES TIMEOUT 60)
add_executable(test_vrlink_validate tests/test_vrlink_validate.cpp src/compositor/foveation_params.cpp)
target_link_libraries(test_vrlink_validate PRIVATE vrhost-core)
add_test(NAME vrlink_validate COMMAND test_vrlink_validate)
# the test headset as a tool, for a running steamac-vrhost (docs/vr.md "Testing without a headset")
add_executable(vrhost-test-headset tests/test_headset_tool.mm tests/fake_headset.cpp tests/vt_decode.mm)
set_source_files_properties(tests/test_headset_tool.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
target_link_libraries(vrhost-test-headset PRIVATE vrhost-core "-framework ImageIO" "-framework CoreGraphics" "-framework Foundation")

# End-to-end game-frame test (wmtr exporter -> link -> compositor -> session -> test headset). The exporter
# (wmtr/src/wmtr_vr.m) needs the WMTVRSubmitInfo layout from a StonkGuy/dxmt vr-export checkout.
set(VRHOST_DXMT_DIR "" CACHE PATH "StonkGuy/dxmt vr-export checkout (enables the vr_e2e test)")
if(VRHOST_DXMT_DIR)
	enable_language(OBJC)
	set(wmtr_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../wmtr)
	add_library(vrhost-test-exporter STATIC ${wmtr_dir}/src/wmtr_vr.m)
	target_include_directories(vrhost-test-exporter PRIVATE include ${wmtr_dir}/include ${CMAKE_CURRENT_SOURCE_DIR}/../../guest/vr/include ${VRHOST_DXMT_DIR}/src/winemetal)
	target_compile_options(vrhost-test-exporter PRIVATE -w)
	add_executable(test_vr_e2e tests/test_vr_e2e.mm tests/fake_headset.cpp tests/vt_decode.mm)
	target_include_directories(test_vr_e2e PRIVATE ${wmtr_dir}/include ${CMAKE_CURRENT_SOURCE_DIR}/../../guest/vr/include ${VRHOST_DXMT_DIR}/src/winemetal)
	set_source_files_properties(tests/test_vr_e2e.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
	target_link_libraries(test_vr_e2e PRIVATE vrhost-core vrhost-compositor vrhost-test-exporter "-framework Metal")
	add_test(NAME vr_e2e COMMAND test_vr_e2e)
	add_executable(test_foveation_geometry tests/test_foveation_geometry.mm)
	set_source_files_properties(tests/test_foveation_geometry.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
	target_link_libraries(test_foveation_geometry PRIVATE vrhost-core vrhost-compositor "-framework Metal" "-framework IOSurface")
	add_test(NAME foveation_geometry COMMAND test_foveation_geometry)
	set_tests_properties(vr_e2e PROPERTIES TIMEOUT 90)
	# the same path with the eyes in one double-wide texture (OpenVR texture bounds) and in one two-slice array
	foreach(layout wide array)
		add_test(NAME vr_e2e_${layout} COMMAND test_vr_e2e)
		set_tests_properties(vr_e2e_${layout} PROPERTIES TIMEOUT 90 ENVIRONMENT VR_E2E_LAYOUT=${layout})
	endforeach()
else()
	message(STATUS "vr_e2e test off: set VRHOST_DXMT_DIR to a StonkGuy/dxmt vr-export checkout")
endif()
endif()

# --- fuzzing (libFuzzer; needs an LLVM clang, not Apple's); each target describes itself in its header ---
# cmake -DVRHOST_FUZZ=ON -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ ... builds every library with
# ASan + UBSan and the targets fuzz_packets (headset message decoder), fuzz_session (TCP stream into a live session)
# and fuzz_seeds (seed corpus writer).
option(VRHOST_FUZZ "Build the libFuzzer targets (instruments everything with ASan + UBSan)" OFF)
if(VRHOST_FUZZ)
	foreach(t fuzz_packets fuzz_session)
		add_executable(${t} tests/fuzz/${t}.cpp)
		target_link_libraries(${t} PRIVATE vrhost-core)
		target_link_options(${t} PRIVATE -fsanitize=fuzzer)
	endforeach()
	add_executable(fuzz_seeds tests/fuzz/fuzz_seeds.cpp)
	target_link_libraries(fuzz_seeds PRIVATE vrhost-core)
endif()

# --- packaging check: the executable must not load OpenSSL (or anything else) from Homebrew ---
if(VRHOST_TESTS AND NOT VRHOST_SYSTEM_OPENSSL)
	add_test(NAME self_contained COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:steamac-vrhost>
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_self_contained.cmake)
endif()
