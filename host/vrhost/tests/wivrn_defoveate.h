// SPDX-License-Identifier: GPL-3.0-or-later
// The WiVRn v26.9 client's defoveation (client/scenes/stream_defoveator.cpp: defoveate() mesh, count_pixels()) on the
// CPU, for tests and the test headset: maps a pixel of the defoveated eye image back to the stream pixel the client
// samples. Parameter i covers p[i] stream pixels, each stretched to |n_ratio - i| + 1 output pixels; within a band the
// client's mesh interpolates linearly, so output coordinate o samples stream coordinate in + (o - out) / ratio.
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace vrhost::test
{
// defoveated size along one axis (stream_defoveator::defoveated_size)
inline uint32_t defoveated_size(const std::vector<uint16_t> & p)
{
	uint32_t n = 0;
	const int mid = int(p.size() - 1) / 2;
	for (size_t i = 0; i < p.size(); ++i)
		n += p[i] * (std::abs(mid - int(i)) + 1);
	return n;
}

// for every defoveated coordinate, the stream pixel the client samples (nearest)
inline std::vector<uint32_t> defoveation_map(const std::vector<uint16_t> & p, uint32_t stream_dim)
{
	std::vector<uint32_t> map;
	map.reserve(defoveated_size(p));
	const int mid = int(p.size() - 1) / 2;
	uint32_t in = 0;
	for (size_t i = 0; i < p.size(); ++i)
	{
		const int ratio = std::abs(mid - int(i)) + 1;
		for (uint32_t o = 0; o < uint32_t(p[i] * ratio); ++o)
		{
			double s = in + (o + 0.5) / ratio;
			uint32_t px = uint32_t(std::floor(s));
			map.push_back(px < stream_dim ? px : stream_dim - 1);
		}
		in += p[i];
	}
	return map;
}

struct Image
{
	uint32_t w = 0, h = 0;
	std::vector<uint8_t> rgb; // 8-bit RGB, row major
	uint8_t luma(uint32_t x, uint32_t y) const
	{
		const uint8_t * p = &rgb[(size_t(y) * w + x) * 3];
		return uint8_t(std::lround(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]));
	}
};

// '420f' (NV12 full range, BT.709, as the compositor writes it) decoded picture -> defoveated RGB eye image, the
// image the WiVRn client reprojects over the frame's view_info FOV
inline Image defoveate(CVPixelBufferRef nv12, const std::vector<uint16_t> & px, const std::vector<uint16_t> & py)
{
	Image img;
	CVPixelBufferLockBaseAddress(nv12, kCVPixelBufferLock_ReadOnly);
	const auto * Y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(nv12, 0);
	const auto * C = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(nv12, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane(nv12, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(nv12, 1);
	uint32_t sw = uint32_t(CVPixelBufferGetWidthOfPlane(nv12, 0)), sh = uint32_t(CVPixelBufferGetHeightOfPlane(nv12, 0));
	auto mx = defoveation_map(px.empty() ? std::vector<uint16_t>{uint16_t(sw)} : px, sw);
	auto my = defoveation_map(py.empty() ? std::vector<uint16_t>{uint16_t(sh)} : py, sh);
	img.w = uint32_t(mx.size());
	img.h = uint32_t(my.size());
	img.rgb.resize(size_t(img.w) * img.h * 3);
	for (uint32_t y = 0; y < img.h; ++y)
		for (uint32_t x = 0; x < img.w; ++x)
		{
			uint32_t sx = mx[x], sy = my[y];
			float l = Y[sy * ys + sx] / 255.f, cb = C[(sy / 2) * cs + (sx / 2) * 2] / 255.f - 0.5f,
			      cr = C[(sy / 2) * cs + (sx / 2) * 2 + 1] / 255.f - 0.5f;
			float rgb[3] = {l + 1.5748f * cr, l - 0.1873f * cb - 0.4681f * cr, l + 1.8556f * cb};
			for (int c = 0; c < 3; ++c)
				img.rgb[(size_t(y) * img.w + x) * 3 + c] = uint8_t(std::lround(std::fmin(std::fmax(rgb[c], 0.f), 1.f) * 255));
		}
	CVPixelBufferUnlockBaseAddress(nv12, kCVPixelBufferLock_ReadOnly);
	return img;
}
} // namespace vrhost::test
