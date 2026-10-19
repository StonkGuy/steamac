// SPDX-License-Identifier: GPL-3.0-or-later
// VideoToolbox low-latency encoder (hardware only). One instance per eye stream. Output is Annex-B with the
// parameter sets in-band before each IDR, as WiVRn clients expect.
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vrhost
{
struct EncoderConfig
{
	enum class Codec
	{
		hevc,
		h264,
	} codec = Codec::hevc;
	uint32_t width = 0, height = 0;
	float fps = 72;
	uint32_t bitrate_bps = 50'000'000;
	// Tuning knobs measured by `--selftest-encoder` (docs/vr.md, encode budget). Defaults = the M0 configuration.
	bool low_latency_rc = true;      // kVTVideoEncoderSpecification_EnableLowLatencyRateControl
	bool prioritize_speed = false;   // kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality
	int max_frame_delay = -1;        // kVTCompressionPropertyKey_MaxFrameDelayCount (-1 = unset)
	int qp = 0;                      // > 0: constant QP via Min/MaxAllowedFrameQP instead of the bitrate target
};

struct EncodedFrame
{
	std::vector<uint8_t> annexb;
	bool idr = false;
	int64_t encode_begin = 0, encode_end = 0; // host clock ns
};

class VtEncoder
{
public:
	// Throws std::runtime_error when no hardware encoder with low-latency rate control is available.
	explicit VtEncoder(const EncoderConfig &);
	~VtEncoder();
	VtEncoder(const VtEncoder &) = delete;
	VtEncoder & operator=(const VtEncoder &) = delete;

	// Synchronous: returns when the frame's bitstream is available. Input: '420f' CVPixelBuffer of the configured size.
	EncodedFrame encode(CVPixelBufferRef, int64_t pts_ns, bool force_idr);
	void set_bitrate(uint32_t bps);

	bool hardware() const;
	bool low_latency() const;
	// Properties VideoToolbox did not accept (for the self-test report).
	const std::vector<std::string> & rejected() const;
	const EncoderConfig & config() const;
	// Pixel buffer pool of the session ('420f', IOSurface-backed, Metal-compatible).
	CVPixelBufferPoolRef pool() const;

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace vrhost
