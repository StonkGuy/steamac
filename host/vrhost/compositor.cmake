# SPDX-License-Identifier: GPL-3.0-or-later
# M1: VR link server + Metal compositor (foveation + NV12), included from host/vrhost/CMakeLists.txt:
#   include(compositor.cmake)
#   target_link_libraries(vrhost-core PUBLIC vrhost-compositor)
# The MSL kernel is a port of WiVRn v26.9 server/compositor/shaders/foveation.comp, and foveation_params.cpp
# of server/compositor/foveation.cpp (both GPL-3.0-or-later, Patrick Nicolas; Collabora BSL-1.0 parts).
add_library(vrhost-compositor STATIC
	src/compositor/foveation_params.cpp
	src/compositor/eye_compositor.mm
	src/compositor/vrlink_server.mm)
target_include_directories(vrhost-compositor PUBLIC src include)
target_link_libraries(vrhost-compositor PUBLIC
	"-framework Foundation" "-framework Metal" "-framework MetalFX" "-framework MetalPerformanceShaders"
	"-framework IOSurface" "-framework CoreVideo")
target_compile_options(vrhost-compositor PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(src/compositor/eye_compositor.mm src/compositor/vrlink_server.mm
	PROPERTIES COMPILE_FLAGS -fobjc-arc)
