// SPDX-License-Identifier: GPL-3.0-or-later
// World-locked test pattern rendered with a Metal compute kernel straight into a '420f' (NV12 full range, BT.709)
// CVPixelBuffer: a sky sphere with a 15-degree grid, coloured 45-degree sectors, a yellow horizon and a red marker at
// world forward (-Z). Rendered for the eye pose/FOV, so on a headset it stays fixed in the world while the head moves.
// The top-left corner carries the frame index as 16 head-locked binary blocks (white = 1) for automated checks.
#pragma once
#include "session/vr_session.h"
#include <CoreVideo/CoreVideo.h>
#include <memory>

namespace vrhost
{
class TestPattern
{
public:
	static constexpr int bit_block = 16; // px per frame-index bit block
	static constexpr int bits = 16;

	TestPattern();
	~TestPattern();
	// Renders and waits for the GPU (encoder input must be complete).
	void render(CVPixelBufferRef nv12, const Pose & eye, const Fov & fov, uint64_t frame_index);
	// Reads the frame index back from a decoded/rendered luma plane (for tests). -1 if unreadable.
	static int64_t read_frame_index(CVPixelBufferRef);

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace vrhost
