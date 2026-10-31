// SPDX-License-Identifier: GPL-3.0-or-later
// VideoToolbox encoder: hardware + low-latency session, Annex-B with VPS/SPS/PPS before IDR only, IDR on request,
// and the bitstream decodes back to the rendered frame (frame index read from the decoded luma).
#include "check.h"
#include "encoder/vt_encoder.h"
#include "pattern/test_pattern.h"
#include "vt_decode.h"
#include "wivrn/annexb.h"

#include <cmath>

using namespace vrhost;

static CVPixelBufferRef make_nv12(uint32_t w, uint32_t h)
{
	CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void * k[] = {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey};
	const void * v[] = {empty, kCFBooleanTrue};
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CVPixelBufferRef buf = nullptr;
	CVPixelBufferCreate(nullptr, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &buf);
	CFRelease(attrs);
	CFRelease(empty);
	return buf;
}

static void run(EncoderConfig::Codec codec)
{
	const bool hevc = codec == EncoderConfig::Codec::hevc;
	VtEncoder enc({.codec = codec, .width = 640, .height = 704, .fps = 72, .bitrate_bps = 10'000'000});
	CHECK(enc.hardware());
	CHECK(enc.low_latency());
	TestPattern pattern;
	VtDecoder dec(hevc);
	CVPixelBufferRef buf = make_nv12(640, 704);
	REQUIRE(buf);

	// rendered pattern carries the frame index
	pattern.render(buf, Pose{}, Fov{}, 0x1234);
	CHECK(TestPattern::read_frame_index(buf) == 0x1234);

	for (int i = 0; i < 20; ++i)
	{
		Pose eye{.orientation = {0, std::sin(i * 0.01f), 0, std::cos(i * 0.01f)}};
		pattern.render(buf, eye, Fov{}, i);
		bool force = i == 10;
		EncodedFrame f = enc.encode(buf, i * 13'888'888, force);
		REQUIRE(not f.annexb.empty());
		CHECK(f.encode_end >= f.encode_begin);
		bool want_idr = i == 0 or i == 10; // infinite GOP: only the first frame and the requested one
		CHECK(f.idr == want_idr);

		auto nals = annexb::split(f.annexb);
		REQUIRE(not nals.empty());
		if (hevc)
		{
			int t0 = annexb::hevc_nal_type(nals[0]);
			if (want_idr)
			{
				REQUIRE(nals.size() >= 4);
				CHECK(t0 == annexb::hevc_vps);
				CHECK(annexb::hevc_nal_type(nals[1]) == annexb::hevc_sps);
				CHECK(annexb::hevc_nal_type(nals[2]) == annexb::hevc_pps);
				bool irap = false;
				for (auto n: nals)
					irap |= annexb::hevc_is_irap(annexb::hevc_nal_type(n));
				CHECK(irap);
			}
			else
			{
				for (auto n: nals)
				{
					int t = annexb::hevc_nal_type(n);
					CHECK(t != annexb::hevc_vps and t != annexb::hevc_sps and t != annexb::hevc_pps);
				}
			}
		}
		CVPixelBufferRef out = dec.decode(f.annexb);
		REQUIRE(out);
		CHECK(CVPixelBufferGetWidth(out) == 640 and CVPixelBufferGetHeight(out) == 704);
		int64_t idx = TestPattern::read_frame_index(out);
		if (idx != i)
			fprintf(stderr, "frame %d decoded as %lld\n", i, (long long)idx);
		CHECK(idx == i);
		CVPixelBufferRelease(out);
	}
	CVPixelBufferRelease(buf);
}

int main()
{
	run(EncoderConfig::Codec::hevc);
	run(EncoderConfig::Codec::h264);
	return test_result("encoder");
}
