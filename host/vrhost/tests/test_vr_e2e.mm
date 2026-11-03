// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end game-frame path, headless, one process:
//   producer (stands in for d3d11bench --vr: two sRGB eye render targets + MTLSharedEvent, as DXMT's
//   IMTLD3D11ExportableTexture hands them out) -> wmtr_vr_submit (the exporter call 146 runs in steamac-vm)
//   -> VR link (SOCK_DGRAM socketpair + Mach IOSurface ports) -> VrPipeline (VrLinkServer -> CompositorBridge ->
//   SessionEncoderSink) -> WivrnSession (VtEncoder, shards) -> loopback test headset (WiVRn client sequence,
//   VideoToolbox decode).
// Checks: decoded frames carry the producer's frame number (index blocks, identity foveation) and the eye's colour
// (left red, right green, like d3d11bench --vr); with a stream smaller than the eye image the foveation parameters
// in view_info are the compositor's (WiVRn-consistent) and the eye colours survive. Prints submit -> decoded latency.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "check.h"
#include "pattern/test_pattern.h"
#include "fake_headset.h"
#include "pipeline/vr_pipeline.h"
#include "session/clock.h"
#include "session/pose_history.h"
#include "wivrn/input_map.h"
#include "wivrn_packets.h"
#include "wivrn/wivrn_session.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
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

// eye colour (linear, as d3d11bench --vr clears its eye targets) + 16 frame-index blocks of 16 px, top-left, the
// layout TestPattern::read_frame_index reads
static const char * kProducer = R"(
#include <metal_stdlib>
using namespace metal;
// arg: eye, frame, x offset of the eye in the texture (double-wide), eye width
kernel void eye(texture2d<float, access::write> t [[texture(0)]], constant uint4 &arg [[buffer(0)]],
                uint2 g [[thread_position_in_grid]]) {
	if (g.x >= arg.w || g.y >= t.get_height()) return;
	uint eye = arg.x, frame = arg.y;
	float4 c = eye ? float4(0.05, 0.5, 0.08, 1) : float4(0.5, 0.05, 0.08, 1);
	if (g.y < 16 && g.x < 256) c = ((frame >> (g.x / 16)) & 1u) ? float4(1) : float4(0, 0, 0, 1);
	t.write(c, uint2(g.x + arg.z, g.y));
}
)";

// How the game lays out its two eyes (OpenVR Submit): one texture per eye; one double-wide texture with
// VRTextureBounds_t halves (Unity single-pass, many native titles); one two-slice array (single-pass instanced).
enum class Layout
{
	separate,
	wide,
	array,
};

struct Producer
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLComputePipelineState> pso;
	// A ring of eye textures, like an OpenVR runtime's copy: the exporter's blit (its own queue, after the event)
	// must not race the next frame's rendering into the same texture (docs/vr-interfaces.md section 3.1).
	static constexpr int ring = 3;
	Layout layout;
	uint32_t w, h;
	id<MTLTexture> tex[ring][2];    // what each eye submits (the same texture twice for wide and array)
	id<MTLTexture> target[ring][2]; // what each eye renders into (a slice view for array)
	id<MTLSharedEvent> ev = [dev newSharedEvent];
	uint64_t ev_value = 0;
	Producer(uint32_t w, uint32_t h, Layout layout) : layout(layout), w(w), h(h)
	{
		NSError * err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@(kProducer) options:nil error:&err];
		pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"eye"] error:&err];
		MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
		                                                                              width:layout == Layout::wide ? 2 * w : w
		                                                                             height:h
		                                                                          mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget |
		          MTLTextureUsagePixelFormatView;
		d.storageMode = MTLStorageModePrivate;
		if (layout == Layout::array)
			d.textureType = MTLTextureType2DArray, d.arrayLength = 2;
		for (int i = 0; i < ring; i++)
		{
			if (layout == Layout::separate)
			{
				for (int e = 0; e < 2; e++)
					tex[i][e] = target[i][e] = [dev newTextureWithDescriptor:d];
				continue;
			}
			id<MTLTexture> t = [dev newTextureWithDescriptor:d];
			for (int e = 0; e < 2; e++)
			{
				tex[i][e] = t;
				target[i][e] = layout == Layout::array ? [t newTextureViewWithPixelFormat:d.pixelFormat
				                                                              textureType:MTLTextureType2D
				                                                                   levels:NSMakeRange(0, 1)
				                                                                   slices:NSMakeRange(e, 1)]
				                                       : t;
			}
		}
	}
	// renders into ring slot frame % ring and returns its eye textures
	id<MTLTexture> const * render(uint32_t frame)
	{
		auto & t = target[frame % ring];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		[ce setComputePipelineState:pso];
		for (uint32_t e = 0; e < 2; e++)
		{
			uint32_t arg[4] = {e, frame, layout == Layout::wide ? e * w : 0, w};
			[ce setTexture:t[e] atIndex:0];
			[ce setBytes:arg length:sizeof arg atIndex:0];
			[ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
		}
		[ce endEncoding];
		[cb encodeSignalEvent:ev value:++ev_value];
		[cb commit];
		return tex[frame % ring];
	}
	// the exporter's view of eye e (what the guest runtime derives from the bounds / array index)
	WMTVREye eye(id<MTLTexture> t, uint32_t e) const
	{
		WMTVREye r{(obj_handle_t)(__bridge void *)t, 0, 0, 0, 0, 0, 0};
		if (layout == Layout::wide)
			r.x = e * w, r.w = w, r.h = h;
		if (layout == Layout::array)
			r.slice = e;
		return r;
	}
};

// Cb/Cr at the image centre (away from the index blocks)
static void centre_chroma(CVPixelBufferRef img, int & cb, int & cr)
{
	CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	auto * p = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 1);
	size_t stride = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
	size_t x = CVPixelBufferGetWidthOfPlane(img, 1) / 2, y = CVPixelBufferGetHeightOfPlane(img, 1) / 2;
	cb = p[y * stride + 2 * x];
	cr = p[y * stride + 2 * x + 1];
	CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}

static double pct(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

// Waits for a condition another thread brings about (the link thread, the exporter's reader, the session): returns
// as soon as it holds; the bound only ends a run in which it never does.
template <typename F>
static bool wait_until(F && cond, std::chrono::milliseconds bound = std::chrono::seconds(10))
{
	auto deadline = std::chrono::steady_clock::now() + bound;
	while (not cond())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

// The exporter for whoever asks for it besides the producer. The producer takes vr_mutex again right after each
// frame; a plain lock_guard here can lose that race for seconds on a busy machine (the mutex is not fair), with the
// headset's receive thread blocked in on_decoded meanwhile. A waiter announces itself and the producer stays out
// until it has had its turn.
struct LinkTurn
{
	std::unique_lock<std::mutex> lock;
	LinkTurn(std::mutex & m, std::atomic<int> & waiters)
	{
		++waiters;
		lock = std::unique_lock(m);
		--waiters;
	}
};

// WiVRn parameter invariants (as m1_pipeline_test): odd length, covers the source, output count = stream size
static bool foveation_consistent(const std::vector<uint16_t> & v, uint32_t src, uint32_t out)
{
	if (v.size() % 2 == 0)
		return false;
	int mid = int(v.size() - 1) / 2;
	uint32_t sum = 0, cnt = 0;
	for (size_t i = 0; i < v.size(); i++)
	{
		sum += v[i] * (std::abs(mid - int(i)) + 1);
		cnt += v[i];
	}
	return sum == src and cnt == out and v[mid] > 0;
}

int main()
{
	@autoreleasepool
	{
		signal(SIGPIPE, SIG_IGN);
		// defaults keep the test short; VR_E2E_SRC / VR_E2E_OUT (WxH) / VR_E2E_FRAMES measure a headset-sized stream
		uint32_t src_w = 640, src_h = 704, out_w = 384, out_h = 416;
		int frames = 60;
		if (const char * e = getenv("VR_E2E_SRC"))
			REQUIRE(sscanf(e, "%ux%u", &src_w, &src_h) == 2);
		if (const char * e = getenv("VR_E2E_OUT"))
			REQUIRE(sscanf(e, "%ux%u", &out_w, &out_h) == 2);
		if (const char * e = getenv("VR_E2E_FRAMES"))
			frames = atoi(e);
		// VR_E2E_LAYOUT=wide|array: the eyes come from one double-wide texture (rects) or one two-slice array
		Layout layout = Layout::separate;
		if (const char * e = getenv("VR_E2E_LAYOUT"))
			layout = !strcmp(e, "wide") ? Layout::wide : !strcmp(e, "array") ? Layout::array : Layout::separate;
		printf("eye layout: %s\n", layout == Layout::wide ? "double-wide" : layout == Layout::array ? "array" : "separate");
		// VR_E2E_FRAMES_IN_FLIGHT=N|auto: the frame clock's hold (--frames-in-flight); default 0
		uint32_t frames_in_flight = 0;
		if (const char * e = getenv("VR_E2E_FRAMES_IN_FLIGHT"))
			frames_in_flight = !strcmp(e, "auto") ? FramesInFlight::adaptive : uint32_t(atoi(e));
		// the real configuration: encryption with PIN pairing (keys in a scratch file, not the user's)
		char dir[] = "/tmp/vr_e2e.XXXXXX";
		REQUIRE(mkdtemp(dir));
		WivrnSession session({.port = 0,
		                      .advertise = false,
		                      .encryption = WivrnConfig::Encryption::pairing,
		                      .pin = "271828",
		                      .known_keys_file = std::string(dir) + "/known_keys",
		                      .log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); }});

		char name[96];
		snprintf(name, sizeof name, "com.steamac.vrhost.e2etest.%d", int(getpid()));
		int sv[2];
		REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
		auto pipeline = std::make_unique<VrPipeline>(session, sv[1], name, CompositorOptions{}, frames_in_flight);
		session.start(pipeline.get()); // the pipeline forwards state / input to the guest
		REQUIRE(session.port() > 0);
		wmtr_vr * vr = wmtr_vr_create(sv[0], name);
		REQUIRE(vr);

		std::mutex m, vr_mutex;
		std::atomic<int> vr_waiters{0}; // LinkTurn
		std::map<uint64_t, int64_t> submit_ns;
		// the head orientation each frame was rendered with (pacer id & 0xffff, as the picture carries it), and the
		// picture id of each decoded left-eye frame by wire index: view_info must carry the render pose, so that the
		// headset's reprojection keeps the scene world-locked
		std::map<uint64_t, std::array<float, 4>> render_orientation;
		std::map<uint64_t, uint64_t> picture_of_wire;
		std::vector<double> latency_ms;
		std::atomic<bool> stop{false};
		std::atomic<int> frames_from_pacer{0}, frames_without_headset{0};
		std::atomic<int64_t> max_sleep_ns{0};
		{
			// before any headset: the guest already gets a usable HMD view (the default Quest 3), not 0x0 / zero FOV
			svr_hello_rep hello{};
			reinterpret_cast<svr_hello_req &>(hello) = {{SVR_HELLO, sizeof(svr_hello_req)}, SVR_VERSION, uint32_t(getpid()), "vr_e2e"};
			CHECK(wmtr_vr_query(vr, &hello, sizeof hello) == 0);
			printf("guest view before any headset: connected=%u eye %ux%u fov L %.2f..%.2f\n", hello.connected, hello.eye_width,
			       hello.eye_height, hello.fov[0][0], hello.fov[0][1]);
			CHECK(not hello.connected and hello.eye_width == 1836 and hello.eye_height == 1924 and hello.fov[0][1] - hello.fov[0][0] > 1);
		}
		std::thread producer([&] {
			Producer prod(src_w, src_h, layout);
			WMTVRSubmitInfo info{};
			uint64_t last_frame = 0;
			while (not stop)
			{
				if (vr_waiters)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				std::lock_guard vr_lock(vr_mutex);
				// what the guest runtime's WaitGetPoses does (guest/vr, M2): SVR_FRAME through the exporter's query, which
				// answers from the host's newest VRL_TIMING / VRL_POSES / VRL_STATE
				svr_frame_rep r{};
				reinterpret_cast<svr_frame_req &>(r) = {{SVR_FRAME, sizeof(svr_frame_req)}, last_frame};
				if (wmtr_vr_query(vr, &r, sizeof r) != 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}
				// As VRChat does: it keeps rendering with no headset (from before the first one, between connections), on
				// the exporter's local cadence; the host drops those frames. The wait must never be long (live
				// 2026-10-17: after a reconnect the game was told to wake ~9000 frames later and froze).
				int64_t sleep_ns = r.wake_up - now_ns();
				max_sleep_ns = std::max<int64_t>(max_sleep_ns, sleep_ns);
				if (not r.connected)
					frames_without_headset++;
				if (sleep_ns > 0)
					std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(sleep_ns, 1'000'000'000)));
				uint64_t frame = r.frame_id; // the pacer's id: WiVRn matches the headset's feedback by it
				last_frame = frame;
				frames_from_pacer++;
				// per-eye pose = head pose +- IPD/2 along head x (as the guest runtime computes it)
				const svr_pose & head = r.pose[SVR_DEV_HEAD];
				Pose hp{.orientation = {head.orientation[0], head.orientation[1], head.orientation[2], head.orientation[3]},
				        .position = {head.position[0], head.position[1], head.position[2]}};
				std::array<Pose, 2> pose;
				std::array<Fov, 2> fov;
				for (int e = 0; e < 2; e++)
				{
					pose[e] = multiply(hp, Pose{.position = {(e ? 0.5f : -0.5f) * r.ipd_m, 0, 0}});
					fov[e] = {r.fov[e][0], r.fov[e][1], r.fov[e][2], r.fov[e][3]};
				}
				auto eye_tex = prod.render(uint32_t(frame));
				for (int e = 0; e < 2; e++)
					info.eye[e] = prod.eye(eye_tex[e], uint32_t(e));
				info.frame_id = frame;
				info.display_time = uint64_t(r.predicted_display);
				for (int e = 0; e < 2; e++)
				{
					float p[7] = {pose[e].orientation[0], pose[e].orientation[1], pose[e].orientation[2], pose[e].orientation[3],
					              pose[e].position[0], pose[e].position[1], pose[e].position[2]};
					float f[4] = {fov[e].left, fov[e].right, fov[e].up, fov[e].down};
					memcpy(info.pose[e], p, sizeof p);
					memcpy(info.fov[e], f, sizeof f);
				}
				info.wait_event = (obj_handle_t)(__bridge void *)prod.ev;
				info.wait_value = prod.ev_value;
				{
					std::lock_guard l(m);
					submit_ns[frame] = now_ns();
					if (head.flags & SVR_POSE_VALID) // frames rendered before tracking arrived carry no head pose to compare
						render_orientation[frame & 0xffff] = {head.orientation[0], head.orientation[1], head.orientation[2],
						                                      head.orientation[3]};
				}
				wmtr_vr_submit(vr, &info);
			}
		});

		int bad_colour = 0, checked = 0, wrong_index = 0, unreadable = 0;
		// The index on the wire counts sent frames; the number in the picture is the pacer's id. They start equal and
		// the picture's runs ahead by one for every frame timing the producer got without a frame reaching the host
		// (wmtr drops a submit while the previous blit is in flight): the lead never shrinks within a connection.
		std::array<int64_t, 2> lead{};
		std::array<bool, 2> lead_set{}; // the first frame of a connection sets the lead (the guest's ids keep counting
		                                // across connections, the host's restart)
		// guest protocol through the link while a headset is connected (once): state, input mapping, haptics
		bool guest_checked = false;
		auto check_guest = [&] {
			LinkTurn turn(vr_mutex, vr_waiters);
			svr_hello_rep hello{};
			reinterpret_cast<svr_hello_req &>(hello) = {{SVR_HELLO, sizeof(svr_hello_req)}, SVR_VERSION, uint32_t(getpid()), "vr_e2e"};
			CHECK(wmtr_vr_query(vr, &hello, sizeof hello) == 0);
			printf("guest view: connected=%u eye %ux%u %.0f Hz ipd %.1f mm name '%s'\n", hello.connected, hello.eye_width,
			       hello.eye_height, hello.refresh_hz, hello.ipd_m * 1000, hello.name);
			CHECK(hello.connected and std::abs(hello.ipd_m - 0.064f) < 0.002f and hello.refresh_hz > 0);
			int hand = -1, idx = -1;
			CHECK(wivrn_input_to_vrl(uint32_t(wivrn::device_id::RIGHT_TRIGGER_VALUE), hand, idx) and hand == 1 and
			      idx == SVR_IN_TRIGGER_VALUE);
			InputValue iv{uint32_t(wivrn::device_id::RIGHT_TRIGGER_VALUE), 0.75f, now_ns()};
			pipeline->on_input(std::span(&iv, 1));
			// the guest's next SVR_FRAME answer (its view of headset and input)
			auto guest_frame = [&] {
				svr_frame_rep fr{};
				reinterpret_cast<svr_frame_req &>(fr) = {{SVR_FRAME, sizeof(svr_frame_req)}, 0};
				CHECK(wmtr_vr_query(vr, &fr, sizeof fr) == 0);
				return fr;
			};
			// WiVRn input -> VRL_INPUT -> the guest's snapshot
			CHECK(wait_until([&] { return guest_frame().input[1][SVR_IN_TRIGGER_VALUE] == 0.75f; }));
			// A new game context has no cached state; HELLO must replay both snapshots.
			int next_fd = dup(sv[0]);
			REQUIRE(next_fd >= 0);
			wmtr_vr_destroy(vr);
			vr = wmtr_vr_create(next_fd, name);
			REQUIRE(vr);
			CHECK(wait_until([&] {
				auto fr = guest_frame();
				return fr.connected and fr.input[1][SVR_IN_TRIGGER_VALUE] == 0.75f;
			}));

			union
			{
				svr_haptic_req req;
				svr_ack ack;
			} hq{.req = {{SVR_HAPTIC, sizeof(svr_haptic_req)}, 1, 0.05f, 0, 0.5f}};
			CHECK(wmtr_vr_query(vr, &hq, sizeof hq) == 0 and hq.ack.status == 0);
			CHECK(wait_until([&] { return pipeline->haptics_routed() != 0; }));
			CHECK(pipeline->haptics_routed() == 1); // SVR_HAPTIC -> VRL_HAPTICS -> VrSession::haptics
			guest_checked = true;
		};
		auto on_decoded = [&](int eye, uint64_t frame, CVPixelBufferRef img) {
			if (not guest_checked and eye == 0 and frame > 10)
				check_guest();
			int64_t read = TestPattern::read_frame_index(img);
			int cb, cr;
			centre_chroma(img, cb, cr);
			// left: red (Cr high, Cb low), right: green (Cr and Cb low)
			bool ok = eye == 0 ? (cr > 160 and cb < 128) : (cr < 110 and cb < 128);
			std::lock_guard l(m);
			checked++;
			if (read < 0)
				unreadable++;
			else
			{
				if (eye == 0)
					picture_of_wire[frame] = uint64_t(read);
				int64_t ahead = (read - int64_t(frame & 0xffff)) & 0xffff;
				if (not lead_set[eye])
					lead[eye] = ahead, lead_set[eye] = true;
				if ((ahead < lead[eye] or ahead > lead[eye] + 100) and wrong_index++ < 3)
					fprintf(stderr, "eye %d frame %llu: carries frame %lld (lead was %lld)\n", eye, (unsigned long long)frame,
					        (long long)read, (long long)lead[eye]);
				else
					lead[eye] = ahead;
			}
			if (not ok and bad_colour++ < 3)
				fprintf(stderr, "eye %d frame %llu: centre Cb %d Cr %d\n", eye, (unsigned long long)frame, cb, cr);
			// only frames whose picture number set the lead: without it the wire index names an unrelated older frame
			if (eye == 1 and lead_set[eye] and submit_ns.count(frame + lead[eye]))
				latency_ms.push_back((now_ns() - submit_ns[frame + lead[eye]]) / 1e6);
		};

		// 1. stream size = eye size: identity foveation, frame numbers readable after the full path
		std::this_thread::sleep_for(std::chrono::milliseconds(400)); // the game runs a while before any headset
		auto r = run_fake_headset({.port = session.port(), .frames = frames, .eye_width = uint16_t(src_w), .eye_height = uint16_t(src_h), .pin = "271828", .on_decoded = on_decoded});
		printf("identity: handshake=%s decoded=%d/%d mismatch=%d/%d idr=%d/%d view_info_ok=%d error='%s'\n",
		       r.handshake_state.c_str(), r.decoded[0], r.decoded[1], r.index_mismatch[0], r.index_mismatch[1], r.idr[0], r.idr[1],
		       r.view_info_ok, r.error.c_str());
		CHECK(r.error.empty());
		CHECK(r.handshake_state == "pin_needed"); // paired with the PIN, then AES-128-CTR on both channels
		CHECK(r.decoded[0] >= 60 and r.decoded[1] >= 60);
		// every readable frame number is the frame's own; a few blocks are blurred right after many bits flip
		printf("identity: frame numbers wrong %d, unreadable %d of %d\n", wrong_index, unreadable, checked);
		CHECK(wrong_index == 0);
		CHECK(unreadable * 20 <= checked);
		CHECK(r.view_info_ok); // identity foveation, eye poses 64 mm apart, display time in headset clock
		CHECK(bad_colour == 0 and checked >= 120);
		{
			// render pose == view_info pose, frame by frame, while the head turns
			std::lock_guard l(m);
			int compared = 0, wrong_pose = 0;
			float min_y = 1, max_y = -1;
			for (auto & [wire, q]: r.view_orientation)
			{
				auto pic = picture_of_wire.find(wire);
				if (pic == picture_of_wire.end() or not render_orientation.count(pic->second))
					continue;
				auto & rq = render_orientation[pic->second];
				compared++;
				min_y = std::min(min_y, rq[1]), max_y = std::max(max_y, rq[1]);
				if (std::abs(q[0] - rq[0]) + std::abs(q[1] - rq[1]) + std::abs(q[2] - rq[2]) + std::abs(q[3] - rq[3]) > 1e-5f and
				    wrong_pose++ < 3)
					fprintf(stderr, "wire frame %llu: view_info orientation y %.4f, rendered with y %.4f\n",
					        (unsigned long long)wire, q[1], rq[1]);
			}
			printf("identity: view_info pose == render pose for %d of %d frames (head yaw quaternion y %.3f..%.3f)\n",
			       compared - wrong_pose, compared, min_y, max_y);
			CHECK(compared >= 30 and wrong_pose == 0);
			CHECK(max_y - min_y > 0.01f); // the head moved during the run
		}
		auto stages = [&](const char * label) {
			auto l = pipeline->take_latency();
			printf("%s: ms p50/p99 submit->copied %.2f/%.2f copied->host %.3f/%.3f host->sent %.2f/%.2f compositor GPU %.2f\n",
			       label, pct(l.submit_to_copied, .5), pct(l.submit_to_copied, .99), pct(l.copied_to_received, .5),
			       pct(l.copied_to_received, .99), pct(l.received_to_sent, .5), pct(l.received_to_sent, .99), pct(l.compose_gpu, .5));
		};
		stages("identity");
		printf("identity: submit -> decoded (both eyes) ms p50 %.2f p99 %.2f max %.2f (%zu frames)\n", pct(latency_ms, .5),
		       pct(latency_ms, .99), pct(latency_ms, 1), latency_ms.size());

		{
			// The headset left: the guest sees no headset and no held input. The session reports the disconnect before
			// its listener has sent VRL_STATE / VRL_INPUT (it tears the encoders down in between), so the guest's own
			// view is what is waited for.
			LinkTurn turn(vr_mutex, vr_waiters);
			CHECK(wait_until([&] {
				svr_frame_rep fr{};
				reinterpret_cast<svr_frame_req &>(fr) = {{SVR_FRAME, sizeof(svr_frame_req)}, 0};
				return wmtr_vr_query(vr, &fr, sizeof fr) == 0 and not fr.connected and fr.input[1][SVR_IN_TRIGGER_VALUE] == 0;
			}));
			CHECK(not session.connected());
		}
		{
			std::lock_guard l(m);
			bad_colour = checked = wrong_index = unreadable = 0;
			lead = {};
			lead_set = {};
			latency_ms.clear();
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(400)); // between headsets the game keeps rendering
		// 2. smaller stream: the compositor foveates; its parameters reach the headset in view_info
		auto f = run_fake_headset({.port = session.port(), .frames = frames, .eye_width = uint16_t(out_w), .eye_height = uint16_t(out_h),
		                           .pin = "271828", .identity_foveation = false, .on_decoded = on_decoded,
		                           .refresh_hz = 90}); // reconnect: another size and refresh rate
		printf("foveated: decoded=%d/%d idr=%d/%d foveation x %zu entries y %zu entries error='%s'\n", f.decoded[0], f.decoded[1],
		       f.idr[0], f.idr[1], f.foveation_x[0].size(), f.foveation_y[0].size(), f.error.c_str());
		CHECK(f.error.empty());
		CHECK(f.decoded[0] >= 60 and f.decoded[1] >= 60);
		for (int e = 0; e < 2; e++)
		{
			CHECK(f.foveation_x[e].size() > 1 and foveation_consistent(f.foveation_x[e], src_w, out_w));
			CHECK(f.foveation_y[e].size() > 1 and foveation_consistent(f.foveation_y[e], src_h, out_h));
		}
		CHECK(bad_colour == 0 and checked >= 120);
		stages("foveated");
		printf("foveated: submit -> decoded (both eyes) ms p50 %.2f p99 %.2f max %.2f (%zu frames)\n", pct(latency_ms, .5),
		       pct(latency_ms, .99), pct(latency_ms, 1), latency_ms.size());

		CHECK(frames_from_pacer >= 120);
		printf("guest frames without a headset %d, longest wait %.1f ms\n", frames_without_headset.load(), max_sleep_ns / 1e6);
		CHECK(frames_without_headset > 30);              // rendered before the first headset and between the two
		CHECK(max_sleep_ns < 3 * 1'000'000'000ll / 72);  // never more than ~2 frames ahead
		CHECK(pipeline->timings_sent() > 0);
		CHECK(guest_checked);
		stop = true;
		producer.join();
		wmtr_vr_stats st;
		wmtr_vr_get_stats(vr, &st);
		auto & ps = pipeline->stats();
		printf("exporter: submits %llu sent %llu dropped %llu errors %llu; pipeline: received %llu submitted %llu no-headset %llu\n",
		       st.submits, st.sent, st.dropped, st.errors, (unsigned long long)ps.received, (unsigned long long)ps.submitted,
		       (unsigned long long)ps.no_headset);
		CHECK(st.errors == 0);
		// 3. teardown as steamac-vm does it: closing the exporter closes the link, the host sees it
		wmtr_vr_destroy(vr);
		int64_t t_close = now_ns();
		wait_until([&] { return pipeline->peer_closed(); });
		printf("link close seen by the host after %.1f ms\n", (now_ns() - t_close) / 1e6);
		CHECK(pipeline->peer_closed());
		session.stop();
		pipeline.reset();
		std::filesystem::remove_all(dir);
	}
	return test_result("vr_e2e");
}
