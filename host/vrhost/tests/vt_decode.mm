// SPDX-License-Identifier: GPL-3.0-or-later
#include "vt_decode.h"
#include "wivrn/annexb.h"

#include <VideoToolbox/VideoToolbox.h>
#include <vector>

using namespace vrhost;

struct VtDecoder::impl
{
	bool hevc;
	CMVideoFormatDescriptionRef fmt = nullptr;
	VTDecompressionSessionRef session = nullptr;
	CVPixelBufferRef last = nullptr;

	void reset()
	{
		if (session)
		{
			VTDecompressionSessionInvalidate(session);
			CFRelease(session);
			session = nullptr;
		}
		if (fmt)
		{
			CFRelease(fmt);
			fmt = nullptr;
		}
	}
};

VtDecoder::VtDecoder(bool hevc) :
        p(std::make_unique<impl>())
{
	p->hevc = hevc;
}

VtDecoder::~VtDecoder()
{
	p->reset();
}

CVPixelBufferRef VtDecoder::decode(std::span<const uint8_t> annexb_data)
{
	auto nals = annexb::split(annexb_data);
	std::vector<const uint8_t *> ps;
	std::vector<size_t> ps_size;
	std::vector<uint8_t> avcc;
	for (auto n: nals)
	{
		int t = p->hevc ? annexb::hevc_nal_type(n) : (n[0] & 0x1f);
		bool is_ps = p->hevc ? (t == annexb::hevc_vps or t == annexb::hevc_sps or t == annexb::hevc_pps) : (t == 7 or t == 8);
		if (is_ps)
		{
			ps.push_back(n.data());
			ps_size.push_back(n.size());
			continue;
		}
		uint32_t len = uint32_t(n.size());
		uint8_t be[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
		avcc.insert(avcc.end(), be, be + 4);
		avcc.insert(avcc.end(), n.begin(), n.end());
	}
	if (not ps.empty())
	{
		p->reset();
		OSStatus st = p->hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, ps.size(), ps.data(), ps_size.data(), 4, nullptr, &p->fmt)
		                      : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, ps.size(), ps.data(), ps_size.data(), 4, &p->fmt);
		if (st != noErr)
			return nullptr;
		int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
		CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pf);
		const void * k[] = {kCVPixelBufferPixelFormatTypeKey};
		const void * v[] = {n};
		CFDictionaryRef attrs = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		st = VTDecompressionSessionCreate(nullptr, p->fmt, nullptr, attrs, nullptr, &p->session);
		CFRelease(attrs);
		CFRelease(n);
		if (st != noErr)
			return nullptr;
	}
	if (not p->session or avcc.empty())
		return nullptr;

	CMBlockBufferRef block = nullptr;
	CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, avcc.size(), nullptr, nullptr, 0, avcc.size(), 0, &block);
	CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
	CMSampleBufferRef sample = nullptr;
	size_t sz = avcc.size();
	CMSampleBufferCreateReady(nullptr, block, p->fmt, 1, 0, nullptr, 1, &sz, &sample);
	__block CVPixelBufferRef out = nullptr;
	OSStatus st = VTDecompressionSessionDecodeFrameWithOutputHandler(
	        p->session, sample, 0, nullptr,
	        ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
		        if (status == noErr and image)
			        out = CVPixelBufferRetain(image);
	        });
	VTDecompressionSessionWaitForAsynchronousFrames(p->session);
	CFRelease(sample);
	CFRelease(block);
	return st == noErr ? out : nullptr;
}
