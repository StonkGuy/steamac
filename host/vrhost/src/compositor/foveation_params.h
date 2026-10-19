// SPDX-License-Identifier: GPL-3.0-or-later
// Foveation parameters, ported from WiVRn v26.9 server/compositor/foveation.cpp (GPL-3.0-or-later,
// Copyright (C) 2026 Patrick Nicolas) with the Vulkan/Monado types removed. The parameter vectors are the
// exact `to_headset::foveation_parameter` content the WiVRn client uses to undo the foveation, and the
// per-pixel source ranges are WiVRn's shader UBO layout (fill_ubo).
#pragma once
#include <cstdint>
#include <vector>

namespace vrhost
{
struct FoveationParams
{
	// WiVRn wire format (to_headset::foveation_parameter): source-pixel counts per ratio, middle entry 1:1.
	std::vector<uint16_t> x, y;
};

struct EyeFov
{
	float left, right, up, down; // radians, left/down negative
	bool operator==(const EyeFov &) const = default;
};

// Parameters for one eye. src_w/h: eye image size; out_w/h: encoded (foveated) size. eye_x: eye position
// along x in metres (head space; +-IPD/2), used for the convergence point like WiVRn without eye gaze.
// If out >= src on an axis the parameter is the identity {src}.
// largest eye rect side the parameters describe (Metal's texture limit; the wire counts are 16-bit)
inline constexpr uint32_t max_foveation_source = 16384;

FoveationParams compute_foveation(const EyeFov & fov, float eye_x, uint32_t src_w, uint32_t src_h, uint32_t out_w,
                                  uint32_t out_h);

// Expands one axis into out_dim+1 source boundaries (WiVRn fill_ubo): output pixel i averages source
// [b[i], b[i+1]) (reversed when flipped). offset/size: the eye's sub-rect on that axis.
std::vector<uint32_t> foveation_boundaries(const std::vector<uint16_t> & params, bool flip, uint32_t offset,
                                           uint32_t size, uint32_t out_dim);
} // namespace vrhost
