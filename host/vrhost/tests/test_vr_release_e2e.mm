// SPDX-License-Identifier: GPL-3.0-or-later
// Frame release (--frames-in-flight auto) in the full pipeline with a GPU-bound game, headless, one process:
//   producer (a GPU-bound D3D11 game as the guest sees it: WaitGetPoses as the guest runtime does it, CPU work, a
//   frame of GPU work ending in the eye images, submit, then DXMT's export bound of one frame) -> wmtr_vr_submit ->
//   VR link -> VrPipeline -> WivrnSession -> loopback test headset.
// One headset connection; the limit starts off and is switched to auto on the running pipeline, as the control
// socket's "frames-in-flight auto" does. Measured per phase after a warm-up: the game's frame rate (distinct frames
// decoded per second) and pose -> decoded latency (the producer's pose fetch to the headset's decode). With no limit
// a GPU-bound game renders a frame ahead of the GPU, so each frame's pose is a GPU frame older than it needs to be;
// auto must take most of that out without costing frames, as test_adaptive_release shows in simulation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "check.h"
#include "pattern/test_pattern.h"
#include "fake_headset.h"
#include "pipeline/vr_pipeline.h"
#include "session/clock.h"
#include "wivrn/wivrn_session.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define WINEMETAL_API
#include "winemetal_thunks.h"
extern "C" {
#include "steamac_vr_guest.h"
#include "wmtr_vr.h"
}

using namespace vrhost;

// eye: the eye colour + 16 frame-index blocks (TestPattern::read_frame_index), as test_vr_e2e's producer;
// burn: GPU time, the bulk of the frame, before the eye images (a game writes its eye images last)
static const char * kProducer = R"(
#include <metal_stdlib>
using namespace metal;
kernel void eye(texture2d<float, access::write> t [[texture(0)]], constant uint4 &arg [[buffer(0)]],
                uint2 g [[thread_position_in_grid]]) {
	if (g.x >= t.get_width() || g.y >= t.get_height()) return;
	uint eye = arg.x, frame = arg.y;
	float4 c = eye ? float4(0.05, 0.5, 0.08, 1) : float4(0.5, 0.05, 0.08, 1);
	if (g.y < 16 && g.x < 256) c = ((frame >> (g.x / 16)) & 1u) ? float4(1) : float4(0, 0, 0, 1);
	t.write(c, g);
}
kernel void burn(device float *out [[buffer(0)]], constant uint &iters [[buffer(1)]], uint i [[thread_position_in_grid]]) {
	float x = float(i);
	for (uint k = 0; k < iters; k++)
		x = fma(x, 0.9999f, 0.5f);
	out[i] = x;
}
)";

constexpr ns ms = 1'000'000;
constexpr float refresh_hz = 72;
constexpr ns period = ns(1e9 / refresh_hz);
constexpr ns gpu_frame = 30 * ms; // about two display periods: VRChat in VR on an M2 (live: 24-36 fps)
constexpr ns cpu_frame = 2 * ms;
constexpr uint32_t burn_threads = 1 << 16;
constexpr int warmup = 30, measured = 90; // decoded frames per phase

struct Producer
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLComputePipelineState> eye_pso, burn_pso;
	id<MTLBuffer> scratch;
	static constexpr int ring = 3; // as an OpenVR runtime's copy (test_vr_e2e)
	id<MTLTexture> tex[ring][2];
	id<MTLSharedEvent> ev = [dev newSharedEvent];
	uint64_t ev_value = 0;
	uint32_t iters = 1000, dispatches = 1;
	Producer(uint32_t w, uint32_t h)
	{
		NSError * err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@(kProducer) options:nil error:&err];
		eye_pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"eye"] error:&err];
		burn_pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"burn"] error:&err];
		scratch = [dev newBufferWithLength:burn_threads * sizeof(float) options:MTLResourceStorageModePrivate];
		MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
		                                                                              width:w
		                                                                             height:h
		                                                                          mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		d.storageMode = MTLStorageModePrivate;
		for (int i = 0; i < ring; i++)
			for (int e = 0; e < 2; e++)
				tex[i][e] = [dev newTextureWithDescriptor:d];
	}
	void encode_burn(id<MTLComputeCommandEncoder> ce, uint32_t n)
	{
		[ce setComputePipelineState:burn_pso];
		[ce setBuffer:scratch offset:0 atIndex:0];
		[ce setBytes:&iters length:sizeof iters atIndex:1];
		for (uint32_t i = 0; i < n; i++)
			[ce dispatchThreads:MTLSizeMake(burn_threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
	}
	// GPU time of `n` burn dispatches with `iters`, the median of a few
	double burn_ms(uint32_t n)
	{
		std::vector<double> t;
		for (int k = 0; k < 5; k++)
		{
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
			encode_burn(ce, n);
			[ce endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			t.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
		}
		std::sort(t.begin(), t.end());
		return t[t.size() / 2];
	}
	// dispatches of about 1 ms each (a game's passes, not one long kernel the compositor would wait behind) adding up
	// to gpu_frame; scaled on the whole frame, since a command buffer's fixed cost would skew a single dispatch
	void calibrate()
	{
		dispatches = uint32_t(std::max<ns>(1, gpu_frame / ms));
		double t = 0;
		for (int k = 0; k < 5; k++)
		{
			t = std::max(burn_ms(dispatches), 0.01);
			iters = uint32_t(std::clamp(iters * (gpu_frame / 1e6) / t, 10.0, 1e7));
		}
		printf("producer GPU frame: %u dispatches x %u iterations = %.1f ms (display period %.1f ms)\n", dispatches, iters,
		       burn_ms(dispatches), period / 1e6);
	}
	// the frame's GPU work, eye images last, into ring slot frame % ring
	id<MTLCommandBuffer> render(uint32_t frame)
	{
		auto & t = tex[frame % ring];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		encode_burn(ce, dispatches);
		[ce setComputePipelineState:eye_pso];
		for (uint32_t e = 0; e < 2; e++)
		{
			uint32_t arg[4] = {e, frame, 0, 0};
			[ce setTexture:t[e] atIndex:0];
			[ce setBytes:arg length:sizeof arg atIndex:0];
			[ce dispatchThreads:MTLSizeMake(t[e].width, t[e].height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
		}
		[ce endEncoding];
		[cb encodeSignalEvent:ev value:++ev_value];
		[cb commit];
		return cb;
	}
};

static double pct(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

static void sleep_until(ns t)
{
	ns now = now_ns();
	if (t > now)
		std::this_thread::sleep_for(std::chrono::nanoseconds(t - now));
}

int main()
{
	@autoreleasepool
	{
		signal(SIGPIPE, SIG_IGN);
		constexpr uint32_t w = 640, h = 704;
		char dir[] = "/tmp/vr_release_e2e.XXXXXX";
		REQUIRE(mkdtemp(dir));
		WivrnSession session({.port = 0,
		                      .advertise = false,
		                      .encryption = WivrnConfig::Encryption::disabled,
		                      .known_keys_file = std::string(dir) + "/known_keys",
		                      .log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); }});
		char name[96];
		snprintf(name, sizeof name, "com.steamac.vrhost.releasetest.%d", int(getpid()));
		int sv[2];
		REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
		auto pipeline = std::make_unique<VrPipeline>(session, sv[1], name, CompositorOptions{}, 0);
		session.start(pipeline.get());
		REQUIRE(session.port() > 0);
		wmtr_vr * vr = wmtr_vr_create(sv[0], name);
		REQUIRE(vr);

		std::mutex m;
		std::map<uint32_t, ns> pose_ns; // frame & 0xffff (what the picture carries) -> when its pose was fetched
		std::atomic<bool> stop{false};
		Producer prod(w, h);
		prod.calibrate();
		std::thread producer([&] {
			WMTVRSubmitInfo info{};
			uint64_t last = 0;
			id<MTLCommandBuffer> previous = nil;
			auto fetch = [&](svr_frame_rep & r) {
				memset(&r, 0, sizeof r);
				reinterpret_cast<svr_frame_req &>(r) = {{SVR_FRAME, sizeof(svr_frame_req)}, last};
				return wmtr_vr_query(vr, &r, sizeof r) == 0;
			};
			while (not stop)
			{
				// WaitGetPoses as the guest runtime does it (guest/vr/runtime/runtime.cpp wait_frame): sleep until the
				// frame's wake-up; while the host's pacer runs but its next frame is not out (held), wait for it
				svr_frame_rep r;
				if (not fetch(r))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}
				sleep_until(r.wake_up);
				fetch(r);
				ns give_up = now_ns() + 200 * ms;
				while ((r.flags & SVR_FRAME_PACER_LIVE) and not(r.flags & SVR_FRAME_FROM_PACER) and now_ns() < give_up and not stop)
				{
					sleep_until(std::max<ns>(r.wake_up, now_ns() + 250'000));
					fetch(r);
				}
				last = r.frame_id;
				{
					std::lock_guard l(m);
					pose_ns[uint32_t(r.frame_id & 0xffff)] = now_ns();
				}
				std::this_thread::sleep_for(std::chrono::nanoseconds(cpu_frame)); // the game's CPU work
				id<MTLCommandBuffer> cb = prod.render(uint32_t(r.frame_id));
				auto & t = prod.tex[uint32_t(r.frame_id) % Producer::ring];
				for (int e = 0; e < 2; e++)
				{
					info.eye[e] = {(obj_handle_t)(__bridge void *)t[e], 0, 0, 0, 0, 0, 0};
					float p[7] = {r.pose[SVR_DEV_HEAD].orientation[0], r.pose[SVR_DEV_HEAD].orientation[1],
					              r.pose[SVR_DEV_HEAD].orientation[2], r.pose[SVR_DEV_HEAD].orientation[3], 0, 0, 0};
					float f[4] = {r.fov[e][0], r.fov[e][1], r.fov[e][2], r.fov[e][3]};
					memcpy(info.pose[e], p, sizeof p);
					memcpy(info.fov[e], f, sizeof f);
				}
				info.frame_id = r.frame_id;
				info.display_time = uint64_t(r.predicted_display);
				info.wait_event = (obj_handle_t)(__bridge void *)prod.ev;
				info.wait_value = prod.ev_value;
				wmtr_vr_submit(vr, &info);
				// DXMT's export bound (d3d11.exportMaxFrameLatency = 1): after presenting a frame the game waits until
				// the frame before it has finished on the GPU
				if (previous)
					[previous waitUntilCompleted];
				previous = cb;
			}
			if (previous)
				[previous waitUntilCompleted];
		});

		struct Phase
		{
			std::vector<double> latency_ms;
			std::set<uint32_t> frames;
			ns first = 0, last = 0;
			PipelineLatency stages;
		};
		Phase off, autop;
		FramesInFlight::Estimate estimate{};
		int decoded = 0;
		auto phase_of = [&](int n) -> Phase * {
			if (n >= warmup and n < warmup + measured)
				return &off;
			if (n >= 2 * warmup + measured and n < 2 * (warmup + measured))
				return &autop;
			return nullptr;
		};
		auto on_decoded = [&](int eye, uint64_t, CVPixelBufferRef img) {
			if (eye != 0)
				return;
			int64_t read = TestPattern::read_frame_index(img);
			ns now = now_ns();
			std::lock_guard l(m);
			int n = decoded++;
			if (n == warmup)
				pipeline->take_latency(); // stage times from here on
			if (n == warmup + measured)
			{
				off.stages = pipeline->take_latency();
				pipeline->set_frames_in_flight(FramesInFlight::adaptive); // the control socket's "frames-in-flight auto"
			}
			if (n == 2 * warmup + measured)
				pipeline->take_latency();
			if (n == 2 * (warmup + measured) - 1)
			{
				autop.stages = pipeline->take_latency();
				estimate = pipeline->release_estimate();
			}
			Phase * p = phase_of(n);
			if (not p or read < 0)
				return;
			auto it = pose_ns.find(uint32_t(read));
			if (it != pose_ns.end())
				p->latency_ms.push_back((now - it->second) / 1e6);
			p->frames.insert(uint32_t(read));
			if (not p->first)
				p->first = now;
			p->last = now;
		};
		auto r = run_fake_headset({.port = session.port(),
		                           .frames = 2 * (warmup + measured),
		                           .eye_width = uint16_t(w),
		                           .eye_height = uint16_t(h),
		                           .timeout_ms = 90000,
		                           .on_decoded = on_decoded,
		                           .refresh_hz = refresh_hz});
		stop = true;
		producer.join();
		printf("headset: decoded=%d/%d error='%s'\n", r.decoded[0], r.decoded[1], r.error.c_str());
		CHECK(r.error.empty());

		auto report = [&](const char * label, const Phase & p) {
			double secs = (p.last - p.first) / 1e9;
			double fps = secs > 0 ? (p.frames.size() - 1) / secs : 0;
			printf("%s: game %.1f fps, pose -> decoded ms p50 %.1f p90 %.1f (%zu frames); submit->copied p50 %.1f ms, "
			       "copied->sent p50 %.1f ms\n",
			       label, fps, pct(p.latency_ms, .5), pct(p.latency_ms, .9), p.latency_ms.size(),
			       pct(p.stages.submit_to_copied, .5), pct(p.stages.copied_to_received, .5) + pct(p.stages.received_to_sent, .5));
			return fps;
		};
		double fps_off = report("off ", off), fps_auto = report("auto", autop);
		printf("auto estimate: CPU %.1f ms, GPU %.1f ms, GPU-bound %s\n", estimate.cpu / 1e6, estimate.gpu / 1e6,
		       estimate.holding ? "yes (held)" : "no");
		CHECK(off.latency_ms.size() >= measured * 3 / 4 and autop.latency_ms.size() >= measured * 3 / 4);
		CHECK(fps_off > 0 and fps_off < refresh_hz * 0.75f); // the producer really is GPU-bound
		CHECK(estimate.holding);                              // auto saw it
		CHECK(estimate.gpu > gpu_frame / 2 and estimate.gpu < gpu_frame * 2);
		// M2, 6 runs: latency p50 auto/off 0.74-0.91, game 23.5-29.8 fps against 21.2-22.1 (WiVRn's pacer drops frames
		// behind a queued GPU frame; the hold avoids that too)
		CHECK(pct(autop.latency_ms, .5) < 0.95 * pct(off.latency_ms, .5)); // queueing taken out
		CHECK(fps_auto >= 0.95 * fps_off);                                 // without costing frames

		wmtr_vr_stats st;
		wmtr_vr_get_stats(vr, &st);
		printf("exporter: submits %llu sent %llu dropped %llu errors %llu\n", st.submits, st.sent, st.dropped, st.errors);
		CHECK(st.errors == 0);
		wmtr_vr_destroy(vr);
		session.stop();
		pipeline.reset();
		std::filesystem::remove_all(dir);
	}
	return test_result("vr_release_e2e");
}
