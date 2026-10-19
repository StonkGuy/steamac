// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: foveation parameters from a VRL_SUBMIT's field of view and eye rect (guest-controlled
// through WMTVRSubmit), as the compositor computes them, and the boundary table the shader indexes.
// Build (Homebrew LLVM; Apple clang has no libFuzzer), from host/vrhost:
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc tests/fuzz/fuzz_foveation.cpp \
//     src/compositor/foveation_params.cpp -o fuzz_foveation
#include "compositor/foveation_params.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size)
{
	struct
	{
		float fov[4];
		float eye_x;
		uint32_t src_w, src_h, out_w, out_h, offset;
		uint8_t flip;
	} in{};
	if (size < sizeof in)
		return 0;
	memcpy(&in, data, sizeof in);
	// output sizes are the headset's (bounded by the session); keep the table allocation sane
	in.out_w %= 8193;
	in.out_h %= 8193;
	vrhost::EyeFov fov{in.fov[0], in.fov[1], in.fov[2], in.fov[3]};
	auto p = vrhost::compute_foveation(fov, in.eye_x, in.src_w, in.src_h, in.out_w, in.out_h);
	auto bx = vrhost::foveation_boundaries(p.x, false, in.offset, in.src_w, in.out_w);
	auto by = vrhost::foveation_boundaries(p.y, in.flip & 1, in.offset, in.src_h, in.out_h);
	return int(bx.size() + by.size()) & 0;
}
