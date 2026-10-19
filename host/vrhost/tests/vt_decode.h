// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper: VideoToolbox decoder for the Annex-B stream steamac-vrhost sends (stands in for the headset decoder).
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <memory>
#include <span>

class VtDecoder
{
public:
	explicit VtDecoder(bool hevc = true);
	~VtDecoder();
	// Decodes one access unit (Annex-B). Returns a retained '420f' pixel buffer, or nullptr (e.g. P frame before IDR).
	CVPixelBufferRef decode(std::span<const uint8_t> annexb);

private:
	struct impl;
	std::unique_ptr<impl> p;
};
