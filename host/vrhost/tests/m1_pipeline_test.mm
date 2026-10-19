// SPDX-License-Identifier: GPL-3.0-or-later
// M1 loopback: eye-texture producer -> wmtr VR exporter (vm side, include/wmtr_vr.h) -> VR link (SOCK_DGRAM +
// Mach IOSurface ports) -> VrLinkServer -> EyeCompositor (foveation + NV12) -> encoder stub. No headset,
// no window. Both link ends run in this process (the real ones are steamac-vm and steamac-vrhost); the
// exporter is reached through wmtr_vr_submit, which is what the executor calls for WMTVRSubmit (call 146,
// covered through the ring by wmtr's loopback test).
//
// Checks: identity foveation reproduces the eye pattern's BT.709 full-range Y/Cb/Cr; foveated output has
// WiVRn-consistent parameters (sum of n_source*count == source size, count == output size) and keeps the
// fovea at 1:1; every frame's images come back (VRL_RELEASE). Prints per-frame latency percentiles.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <servers/bootstrap.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "compositor/vrlink_server.h"
#define WINEMETAL_API
#include "winemetal_thunks.h"
extern "C" {
#include "wmtr_vr.h"
}

using namespace vrhost;

static int failures;
#define CHECK(c, ...)                                                                                                  \
	do                                                                                                             \
	{                                                                                                              \
		if (!(c))                                                                                                      \
		{                                                                                                              \
			failures++;                                                                                                \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
			fprintf(stderr, __VA_ARGS__);                                                                              \
			fprintf(stderr, "\n");                                                                                     \
		}                                                                                                              \
	} while (0)

static int64_t now_ns()
{
	return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

// encoder stand-in: '420f' IOSurface-backed pool like VtEncoder::pool(), records what it was given
struct StubEncoder : EyeEncoderSink
{
	CVPixelBufferPoolRef pool = nullptr;
	uint32_t w, h;
	std::mutex m;
	std::vector<int64_t> done_ns;
	uint64_t frames = 0;
	FoveationParams last_params[2];
	std::vector<uint8_t> y[2], cbcr[2]; // copies of the last frame's planes (when keep)
	bool keep = false;

	StubEncoder(uint32_t w, uint32_t h) : w(w), h(h)
	{
		NSDictionary * attrs = @{
			(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
			(id)kCVPixelBufferWidthKey: @(w),
			(id)kCVPixelBufferHeightKey: @(h),
			(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
			(id)kCVPixelBufferMetalCompatibilityKey: @YES,
		};
		CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attrs, &pool);
	}
	~StubEncoder() { CVPixelBufferPoolRelease(pool); }
	CVPixelBufferRef acquire(int) override
	{
		CVPixelBufferRef b = nullptr;
		CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &b);
		return b;
	}
	void encode(int eye, CVPixelBufferRef b, const LinkFrame &, const FoveationParams & p) override
	{
		last_params[eye] = p;
		if (keep)
		{
			CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
			for (int pl = 0; pl < 2; pl++)
			{
				auto & dst = pl ? cbcr[eye] : y[eye];
				size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(b, pl), rows = CVPixelBufferGetHeightOfPlane(b, pl);
				size_t wb = CVPixelBufferGetWidthOfPlane(b, pl) * (pl ? 2 : 1);
				dst.resize(wb * rows);
				auto * src = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(b, pl);
				for (size_t r = 0; r < rows; r++)
					memcpy(&dst[r * wb], src + r * bpr, wb);
			}
			CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
		}
		if (eye == 1)
		{
			std::lock_guard l(m);
			done_ns.push_back(now_ns());
			frames++;
		}
	}
};

// stereo pattern producer (stands in for the guest's DXMT render targets): left eye = colour A with a
// vertical bar, right eye = colour B; the bar moves with the frame number
static const char * kPattern = R"(
#include <metal_stdlib>
using namespace metal;
kernel void pattern(texture2d<float, access::write> t [[texture(0)]], constant uint4 &arg [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]]) {
	if (g.x >= t.get_width() || g.y >= t.get_height()) return;
	uint eye = arg.x, frame = arg.y;
	float4 c = eye ? float4(0.1, 0.8, 0.2, 1) : float4(0.9, 0.2, 0.1, 1);
	if (arg.z && (g.x + frame * 8) % 256 < 16) c = float4(1, 1, 1, 1);
	t.write(c, g);
}
)";

struct Producer
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLComputePipelineState> pso;
	id<MTLTexture> tex[2];
	id<MTLSharedEvent> ev = [dev newSharedEvent];
	uint64_t ev_value = 0;
	Producer(uint32_t w, uint32_t h)
	{
		NSError * err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@(kPattern) options:nil error:&err];
		pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"pattern"] error:&err];
		for (auto & t: tex)
		{
			// sRGB render targets, like Unity's eye textures through DXMT (R8G8B8A8_UNORM_SRGB)
			MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
			                                                                              width:w
			                                                                             height:h
			                                                                          mipmapped:NO];
			d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
			d.storageMode = MTLStorageModePrivate;
			t = [dev newTextureWithDescriptor:d];
		}
	}
	// "render" a frame on the app's queue and signal the event, like DXMT's FlushForExport
	void render(uint32_t frame, bool bar)
	{
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		[ce setComputePipelineState:pso];
		for (uint32_t e = 0; e < 2; e++)
		{
			uint32_t arg[4] = {e, frame, bar ? 1u : 0u, 0};
			[ce setTexture:tex[e] atIndex:0];
			[ce setBytes:arg length:sizeof arg atIndex:0];
			[ce dispatchThreads:MTLSizeMake(tex[e].width, tex[e].height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
		}
		[ce endEncoding];
		[cb encodeSignalEvent:ev value:++ev_value];
		[cb commit];
	}
};

static float srgb_encode(float v)
{
	return v < 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
}
// expected 8-bit Y/Cb/Cr of a linear colour, as the shader computes it (WiVRn's matrix, full range)
static void expect_ycc(float r, float g, float b, int out[3])
{
	// the texture stores sRGB-encoded 8-bit values of the linear colour; sampling decodes, the shader re-encodes
	float R = roundf(srgb_encode(r) * 255) / 255, G = roundf(srgb_encode(g) * 255) / 255, B = roundf(srgb_encode(b) * 255) / 255;
	out[0] = int(lroundf((0.2126f * R + 0.7152f * G + 0.0722f * B) * 255));
	out[1] = int(lroundf((-0.1146f * R - 0.3854f * G + 0.5f * B + 0.5f) * 255));
	out[2] = int(lroundf((0.5f * R - 0.4542f * G - 0.0458f * B + 0.5f) * 255));
}

struct Run
{
	std::vector<double> submit_to_ready, ready_to_recv, recv_to_encoded, total, gpu;
};

static double pct(std::vector<double> v, double p)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

// one link + compositor configuration; returns per-frame timings
static void run_case(const char * label, uint32_t src_w, uint32_t src_h, uint32_t out_w, uint32_t out_h, int nframes,
                     double hz, bool check_identity)
{
	char name[96];
	snprintf(name, sizeof name, "com.steamac.vrhost.m1test.%d.%s", int(getpid()), label);
	int sv[2];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
	StubEncoder enc(out_w, out_h);
	enc.keep = check_identity;
	CompositorBridge bridge(out_w, out_h, enc);
	std::mutex tm;
	Run run;
	std::atomic<uint64_t> last_frame{0};
	VrLinkServer server(sv[1], name, [&](const LinkFrame & f) {
		bridge(f);
		int64_t end = now_ns();
		auto t = bridge.last_timing();
		std::lock_guard l(tm);
		run.submit_to_ready.push_back((f.ready_time - f.app_submit_time) / 1e6);
		run.ready_to_recv.push_back((f.received_time - f.ready_time) / 1e6);
		run.recv_to_encoded.push_back((end - f.received_time) / 1e6);
		run.total.push_back((end - f.app_submit_time) / 1e6);
		run.gpu.push_back(t.compose_gpu_ns / 1e6);
		last_frame = f.frame_id;
	});
	wmtr_vr * vr = wmtr_vr_create(sv[0], name);
	CHECK(vr, "wmtr_vr_create");
	Producer prod(src_w, src_h);
	WMTVRSubmitInfo info{};
	for (int e = 0; e < 2; e++)
	{
		info.eye[e] = WMTVREye{(obj_handle_t)(__bridge void *)prod.tex[e], 0, 0, 0, 0, 0, 0};
		float pose[7] = {0, 0, 0, 1, e ? 0.032f : -0.032f, 1.6f, 0};
		float fov[4] = {e ? -0.75f : -0.87f, e ? 0.87f : 0.75f, 0.82f, -0.9f}; // Quest 3-like, asymmetric
		memcpy(info.pose[e], pose, sizeof pose);
		memcpy(info.fov[e], fov, sizeof fov);
	}
	int64_t period = int64_t(1e9 / hz), next = now_ns();
	for (int i = 1; i <= nframes; i++)
	{
		prod.render(i, !check_identity);
		info.frame_id = i;
		info.display_time = next + 2 * period;
		info.wait_event = (obj_handle_t)(__bridge void *)prod.ev;
		info.wait_value = prod.ev_value;
		CHECK(wmtr_vr_submit(vr, &info) == 0, "submit %d", i);
		next += period;
		int64_t d = next - now_ns();
		if (d > 0)
			usleep(useconds_t(d / 1000));
	}
	for (int i = 0; i < 200 && last_frame < uint64_t(nframes); i++)
		usleep(5000);
	usleep(20000);
	wmtr_vr_stats st;
	wmtr_vr_get_stats(vr, &st);
	CHECK(st.errors == 0, "%s: %llu exporter errors", label, st.errors);
	CHECK(st.sent + st.dropped == uint64_t(nframes), "%s: sent %llu + dropped %llu != %d", label, st.sent, st.dropped, nframes);
	CHECK(enc.frames == st.sent, "%s: encoded %llu of %llu sent", label, enc.frames, st.sent);
	// releases are drained by the exporter on its next submit: the last frame's (2 images) may be pending
	CHECK(st.released + 2 >= 2 * st.sent && st.released <= 2 * st.sent, "%s: released %llu for %llu frames", label, st.released, st.sent);

	if (check_identity)
	{
		// identity: every pixel is the eye colour
		CHECK(enc.last_params[0].x.size() == 1 && enc.last_params[0].x[0] == src_w, "identity params x");
		float col[2][3] = {{0.9f, 0.2f, 0.1f}, {0.1f, 0.8f, 0.2f}};
		for (int e = 0; e < 2; e++)
		{
			int ycc[3];
			expect_ycc(col[e][0], col[e][1], col[e][2], ycc);
			int maxd = 0;
			for (uint8_t v: enc.y[e])
				maxd = std::max(maxd, std::abs(int(v) - ycc[0]));
			for (size_t i = 0; i < enc.cbcr[e].size(); i += 2)
				maxd = std::max({maxd, std::abs(int(enc.cbcr[e][i]) - ycc[1]), std::abs(int(enc.cbcr[e][i + 1]) - ycc[2])});
			CHECK(maxd <= 1, "eye %d: YCbCr off by %d (expect %d,%d,%d)", e, maxd, ycc[0], ycc[1], ycc[2]);
		}
	}
	else
	{
		for (int e = 0; e < 2; e++)
		{
			const auto & p = enc.last_params[e];
			auto check_axis = [&](const std::vector<uint16_t> & v, uint32_t src, uint32_t out, const char * ax) {
				int n_ratio = (int(v.size()) - 1) / 2;
				uint32_t sum = 0, cnt = 0;
				for (size_t i = 0; i < v.size(); i++)
				{
					sum += v[i] * (std::abs(n_ratio - int(i)) + 1);
					cnt += v[i];
				}
				CHECK(v.size() % 2 == 1 && sum == src && cnt == out && v[n_ratio] > 0,
				      "eye %d %s params: %zu entries, covers %u of %u source, %u of %u output", e, ax, v.size(), sum, src, cnt, out);
			};
			check_axis(p.x, src_w, out_w, "x");
			check_axis(p.y, src_h, out_h, "y");
		}
	}
	auto & r = run;
	printf("%-10s src %ux%u -> out %ux%u, %d frames at %.0f Hz: sent %llu dropped %llu\n", label, src_w, src_h, out_w,
	       out_h, nframes, hz, st.sent, st.dropped);
	printf("  ms (p50/p99/max): submit->copy done %.2f/%.2f/%.2f  copy done->host recv %.3f/%.3f/%.3f  "
	       "recv->encoder %.2f/%.2f/%.2f  total %.2f/%.2f/%.2f  compositor GPU %.2f/%.2f\n",
	       pct(r.submit_to_ready, .5), pct(r.submit_to_ready, .99), pct(r.submit_to_ready, 1), pct(r.ready_to_recv, .5),
	       pct(r.ready_to_recv, .99), pct(r.ready_to_recv, 1), pct(r.recv_to_encoded, .5), pct(r.recv_to_encoded, .99),
	       pct(r.recv_to_encoded, 1), pct(r.total, .5), pct(r.total, .99), pct(r.total, 1), pct(r.gpu, .5), pct(r.gpu, .99));
	wmtr_vr_destroy(vr);
}

// A transient encoder error must not kill the link thread or strand its two images.
static void test_sink_failure()
{
	char name[96];
	snprintf(name, sizeof name, "com.steamac.vrhost.m1test.%d.error", int(getpid()));
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair");
	std::atomic<int> calls{0}, resets{0};
	VrLinkServer server(sv[1], name, [&](const LinkFrame &) {
		if (++calls == 1)
			throw std::runtime_error("test encoder failure");
	}, {}, [&] { resets++; });
	wmtr_vr * vr = wmtr_vr_create(sv[0], name);
	CHECK(vr, "wmtr_vr_create");
	Producer prod(64, 64);
	WMTVRSubmitInfo info{};
	for (int e = 0; e < 2; e++)
		info.eye[e] = WMTVREye{(obj_handle_t)(__bridge void *)prod.tex[e], 0, 0, 0, 0, 0, 0};
	info.wait_event = (obj_handle_t)(__bridge void *)prod.ev;
	for (int i = 1; i <= 3; i++)
	{
		prod.render(i, false);
		info.frame_id = i;
		info.wait_value = prod.ev_value;
		CHECK(wmtr_vr_submit(vr, &info) == 0, "submit after sink error %d", i);
		usleep(20000);
	}
	wmtr_vr_stats st{};
	for (int i = 0; i < 200; i++)
	{
		wmtr_vr_get_stats(vr, &st);
		if (calls >= 2 and st.released >= 2 * st.sent - 2)
			break;
		usleep(5000);
	}
	CHECK(calls >= 2, "link thread survived transient sink error, calls %d", calls.load());
	CHECK(resets == 1, "sink reset once after encoder failure, got %d", resets.load());
	CHECK(st.sent >= 2 and st.released + 2 >= 2 * st.sent, "released %llu of %llu sent frames after error",
	      st.released, st.sent);
	wmtr_vr_destroy(vr);
}

int main(int argc, char ** argv)
{
	@autoreleasepool
	{
		test_sink_failure();
		int frames = argc > 1 ? atoi(argv[1]) : 300;
		// identity (no foveation): exact colour conversion
		run_case("identity", 512, 512, 512, 512, 10, 90, true);
		// Quest 3 class: WiVRn render size ~2064x2208 per eye, stream at ~0.6x linear
		run_case("quest3", 2064, 2208, 1248, 1344, frames, 90, false);
		// Quest 2 / VRChat-typical render size, 72 Hz
		run_case("quest2", 1832, 1920, 1440, 1536, frames / 2, 72, false);
	}
	printf(failures ? "m1_pipeline_test: %d FAILED\n" : "m1_pipeline_test: all passed\n", failures);
	return failures != 0;
}
