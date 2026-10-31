// SPDX-License-Identifier: GPL-3.0-or-later
// Checks on what the vm side of the VR link sends (vrlink.h). The vm side runs the guest's submits through the
// wmtr exporter, so field of view, poses and the frame id are the guest's own values: the host takes nothing
// from a packet that it has not checked against the swapchain it already holds.
#pragma once
#include "vrlink.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vrhost
{
// largest swapchain image side (Metal's texture limit)
inline constexpr uint32_t vrl_max_image_side = 16384;

// bytes per element of the formats the exporter creates (wmtr_vr.m format_info), 0 = not accepted
inline uint32_t vrl_format_bpe(uint32_t mtl_format)
{
	switch (mtl_format)
	{
		case 70: // MTLPixelFormatRGBA8Unorm
		case 71: // MTLPixelFormatRGBA8Unorm_sRGB
		case 80: // MTLPixelFormatBGRA8Unorm
		case 81: // MTLPixelFormatBGRA8Unorm_sRGB
			return 4;
		case 115: // MTLPixelFormatRGBA16Float
			return 8;
		default:
			return 0;
	}
}

inline bool vrl_swapchain_ok(const vrl_swapchain & s)
{
	return s.id != 0 and s.image_count >= 1 and s.image_count <= VRL_MAX_IMAGES and s.width >= 1 and
	       s.width <= vrl_max_image_side and s.height >= 1 and s.height <= vrl_max_image_side and
	       vrl_format_bpe(s.pixel_format) != 0;
}

// an image the vm side passed for a swapchain must hold what the description says
inline bool vrl_surface_ok(const vrl_swapchain & s, size_t width, size_t height, size_t bytes_per_element)
{
	return width >= s.width and height >= s.height and bytes_per_element == vrl_format_bpe(s.pixel_format);
}

// the eye's sub-rect lies inside the swapchain's images (64-bit sums: no wrap-around)
inline bool vrl_eye_rect_ok(const vrl_eye & e, const vrl_swapchain & s)
{
	return e.w >= 1 and e.h >= 1 and uint64_t(e.x) + e.w <= s.width and uint64_t(e.y) + e.h <= s.height;
}

// left < right, down < up, all finite and inside +-90 degrees. A frame whose FOV fails this is composed with the
// headset's own FOV (VrPipeline treats a zero FOV that way), not with the guest's values.
inline bool vrl_fov_ok(const float f[4])
{
	const float lim = 1.5707f;
	for (int i = 0; i < 4; i++)
		if (not std::isfinite(f[i]) or std::fabs(f[i]) > lim)
			return false;
	return f[0] < f[1] and f[3] < f[2];
}

inline bool vrl_poses_ok(const vrl_submit & s)
{
	for (auto & eye: s.pose)
		for (float v: eye)
			if (not std::isfinite(v))
				return false;
	return true;
}
} // namespace vrhost
