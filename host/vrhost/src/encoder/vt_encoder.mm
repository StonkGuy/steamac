// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder/vt_encoder.h"
#include "session/clock.h"
#include "wivrn/annexb.h"

#include <VideoToolbox/VideoToolbox.h>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace vrhost
{
struct VtEncoder::impl
{
	EncoderConfig cfg;
	VTCompressionSessionRef session = nullptr;
	bool hw = false;
	bool lowlat = false;
	std::vector<std::string> rejected;

	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	OSStatus status = noErr;
	EncodedFrame out;

	void on_output(OSStatus st, CMSampleBufferRef sample)
	{
		std::lock_guard lock(mutex);
		status = st;
		if (st == noErr and sample)
			convert(sample);
		done = true;
		cv.notify_all();
	}

	void convert(CMSampleBufferRef sample)
	{
		out.annexb.clear();
		out.idr = true;
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sample, false);
		if (att and CFArrayGetCount(att) > 0)
		{
			auto dict = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
			CFBooleanRef not_sync = nullptr;
			if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, (const void **)&not_sync))
				out.idr = not CFBooleanGetValue(not_sync);
		}

		CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
		int length_size = 4;
		size_t count = 0;
		const uint8_t * ps;
		size_t ps_size;
		if (cfg.codec == EncoderConfig::Codec::hevc)
			CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, &ps, &ps_size, &count, &length_size);
		else
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &ps, &ps_size, &count, &length_size);
		if (out.idr)
		{
			for (size_t i = 0; i < count; ++i)
			{
				OSStatus s = cfg.codec == EncoderConfig::Codec::hevc
				                     ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &ps, &ps_size, nullptr, nullptr)
				                     : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &ps_size, nullptr, nullptr);
				if (s == noErr)
					annexb::append_nal(out.annexb, {ps, ps_size});
			}
		}

		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
		size_t total = CMBlockBufferGetDataLength(block);
		std::vector<uint8_t> tmp(total);
		CMBlockBufferCopyDataBytes(block, 0, total, tmp.data());
		if (not annexb::append_length_prefixed(out.annexb, tmp, length_size))
			status = kVTVideoEncoderMalfunctionErr;
	}
};

static void set_prop(VTCompressionSessionRef s, CFStringRef key, CFTypeRef value, bool required)
{
	OSStatus st = VTSessionSetProperty(s, key, value);
	if (st != noErr and required)
		throw std::runtime_error("VTSessionSetProperty " + std::string(CFStringGetCStringPtr(key, kCFStringEncodingUTF8) ?: "?") + " failed: " + std::to_string(st));
}

static void set_num(VTCompressionSessionRef s, CFStringRef key, double v, bool required)
{
	CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
	set_prop(s, key, n, required);
	CFRelease(n);
}

VtEncoder::VtEncoder(const EncoderConfig & cfg) :
        p(std::make_unique<impl>())
{
	p->cfg = cfg;
	const void * spec_keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
	                            kVTVideoEncoderSpecification_EnableLowLatencyRateControl};
	const void * spec_values[] = {kCFBooleanTrue, cfg.low_latency_rc ? kCFBooleanTrue : kCFBooleanFalse};
	CFDictionaryRef spec = CFDictionaryCreate(nullptr, spec_keys, spec_values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	int32_t pixfmt = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
	CFNumberRef pf = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pixfmt);
	CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void * src_keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey};
	const void * src_values[] = {pf, empty, kCFBooleanTrue};
	CFDictionaryRef src = CFDictionaryCreate(nullptr, src_keys, src_values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CMVideoCodecType codec = cfg.codec == EncoderConfig::Codec::hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
	OSStatus st = VTCompressionSessionCreate(nullptr, cfg.width, cfg.height, codec, spec, src, nullptr, nullptr, nullptr, &p->session);
	CFRelease(spec);
	CFRelease(src);
	CFRelease(empty);
	CFRelease(pf);
	if (st != noErr)
		throw std::runtime_error("no hardware low-latency " + std::string(cfg.codec == EncoderConfig::Codec::hevc ? "HEVC" : "H.264") + " encoder for " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + " (VTCompressionSessionCreate " + std::to_string(st) + ")");

	auto s = p->session;
	set_prop(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, false);
	set_prop(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, true);
	set_prop(s, kVTCompressionPropertyKey_ProfileLevel,
	         cfg.codec == EncoderConfig::Codec::hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel, false);
	// Infinite GOP: IDR only on request (WiVRn's IDR handler model).
	set_num(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, INT32_MAX, false);
	set_num(s, kVTCompressionPropertyKey_ExpectedFrameRate, cfg.fps, false);
	set_num(s, kVTCompressionPropertyKey_AverageBitRate, cfg.bitrate_bps, false);
	// BT.709 full range, matching WiVRn's server colour conversion (foveation.comp) and client sampler setup.
	set_prop(s, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, false);
	set_prop(s, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2, false);
	set_prop(s, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, false);

	auto opt = [&](CFStringRef key, CFTypeRef v, const char * name) {
		if (VTSessionSetProperty(s, key, v) != noErr)
			p->rejected.push_back(name);
	};
	auto opt_num = [&](CFStringRef key, double v, const char * name) {
		CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
		opt(key, n, name);
		CFRelease(n);
	};
	if (cfg.prioritize_speed)
		opt(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue, "PrioritizeEncodingSpeedOverQuality");
	if (cfg.max_frame_delay >= 0)
		opt_num(kVTCompressionPropertyKey_MaxFrameDelayCount, cfg.max_frame_delay, "MaxFrameDelayCount");
	if (cfg.qp > 0)
	{
		opt_num(kVTCompressionPropertyKey_MinAllowedFrameQP, cfg.qp, "MinAllowedFrameQP");
		opt_num(kVTCompressionPropertyKey_MaxAllowedFrameQP, cfg.qp, "MaxAllowedFrameQP");
	}

	// RequireHardwareAcceleratedVideoEncoder makes session creation fail without a hardware encoder. The read-back
	// property is not supported by every encoder (the low-latency HEVC one returns an error), so only an explicit
	// "false" counts as software.
	p->hw = true;
	CFBooleanRef hw = nullptr;
	OSStatus hs = VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &hw);
	if (hs == noErr and hw)
	{
		p->hw = CFBooleanGetValue(hw);
		CFRelease(hw);
	}
	// Session creation fails when EnableLowLatencyRateControl cannot be honoured, so a session = low-latency RC.
	p->lowlat = cfg.low_latency_rc;
	if (not p->hw)
		throw std::runtime_error("VideoToolbox picked a software encoder; hardware is required");
	VTCompressionSessionPrepareToEncodeFrames(s);
}

VtEncoder::~VtEncoder()
{
	if (p->session)
	{
		VTCompressionSessionInvalidate(p->session);
		CFRelease(p->session);
	}
}

EncodedFrame VtEncoder::encode(CVPixelBufferRef buf, int64_t pts_ns, bool force_idr)
{
	{
		std::lock_guard lock(p->mutex);
		p->done = false;
		p->status = noErr;
		p->out = {};
	}
	int64_t begin = now_ns();
	CFDictionaryRef props = nullptr;
	if (force_idr)
	{
		const void * k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
		const void * v[] = {kCFBooleanTrue};
		props = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	impl * self = p.get();
	OSStatus st = VTCompressionSessionEncodeFrameWithOutputHandler(
	        p->session, buf, CMTimeMake(pts_ns, 1'000'000'000), kCMTimeInvalid, props, nullptr,
	        ^(OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample) {
		        self->on_output(status, sample);
	        });
	if (props)
		CFRelease(props);
	if (st != noErr)
		throw std::runtime_error("VTCompressionSessionEncodeFrame failed: " + std::to_string(st));

	std::unique_lock lock(p->mutex);
	p->cv.wait(lock, [&] { return p->done; });
	if (p->status != noErr)
		throw std::runtime_error("encode failed: " + std::to_string(p->status));
	EncodedFrame r = std::move(p->out);
	r.encode_begin = begin;
	r.encode_end = now_ns();
	return r;
}

void VtEncoder::set_bitrate(uint32_t bps)
{
	set_num(p->session, kVTCompressionPropertyKey_AverageBitRate, bps, false);
	p->cfg.bitrate_bps = bps;
}

bool VtEncoder::hardware() const { return p->hw; }
bool VtEncoder::low_latency() const { return p->lowlat; }
const std::vector<std::string> & VtEncoder::rejected() const { return p->rejected; }
const EncoderConfig & VtEncoder::config() const { return p->cfg; }
CVPixelBufferPoolRef VtEncoder::pool() const { return VTCompressionSessionGetPixelBufferPool(p->session); }
} // namespace vrhost
