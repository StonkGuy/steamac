// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: the stream under the conditions of the 2026-10-17 headset trial, against the test headset
// (fake_vd_client). The producer works like the host pipeline (VrPipeline::frame_clock + on_frame): poses from
// pose_at() right after next_frame_timing(), a render delay, then submit() with the pacer's frame id.
//   1. world lock: each frame's FrameMetadata.frame_index names the tracking sample its pose came from (the headset
//      reprojects from that sample; a newer index makes the picture follow the head);
//   2. the headset toggling spacewarp mid-stream (render divisor 1 <-> 2): frames keep flowing and decoding;
//   3. a headset that stops reading the video channel for 3 s: submit() never blocks the caller (in the host it is
//      the VR link thread that returns the game's eye images), and the stream recovers with a key frame;
//   4. a corrupt/sleeping controller pose (zero quaternion, NaN) is reported untracked instead of extrapolated.
// Discovery key: random per run (never a real key).
#include "check.h"
#include "fake_vd_client.h"
#include "pattern_server.h"
#include "vd/vd_session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <map>
#include <set>
#include <thread>

using namespace vrhost;
using namespace vrhost::vd;

namespace
{
constexpr float yaw_step = 0.003f; // head yaw per tracking sample (rad)

struct Listener : SessionListener
{
	std::mutex m;
	std::vector<uint32_t> divisors;
	std::optional<TrackedPose> left;
	void on_render_rate(uint32_t d) override
	{
		std::lock_guard l(m);
		divisors.push_back(d);
	}
	void on_tracking(std::span<const TrackedPose> p) override
	{
		std::lock_guard l(m);
		for (auto & t: p)
			if (t.device == Device::left_grip)
				left = t;
	}
};

TrackingSample sample(int32_t n)
{
	TrackingSample s;
	s.head.tracked = true;
	float a = yaw_step * float(n);
	s.head.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
	s.head.position = {0, 1.6f, 0};
	s.head.angular_velocity = {0, yaw_step / 0.002f, 0}; // turning at what the samples show (2 ms apart)
	return s;
}

float yaw(const Quat & q) // rotation about +y only
{
	return 2 * std::atan2(q.y, q.w);
}

float yaw_err(const Quat & q, int32_t n) // |yaw(q) - yaw of sample n|, wrapped to [0, pi]
{
	float d = std::remainder(yaw(q) - yaw_step * float(n), float(2 * M_PI));
	return std::abs(d);
}
} // namespace

int main()
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	char dir_template[] = "/tmp/vrhost-vds.XXXXXX";
	REQUIRE(mkdtemp(dir_template) != nullptr);
	std::string dir = dir_template;
	AesKey disc = AesKey::random();

	Listener listener;
	VdSession session({
	        .control_port = 0,
	        .data_port = 0,
	        .video_port = 0,
	        .audio_port = 0,
	        .discovery_port = 0,
	        .discovery_key = disc,
	        .accounts_file = dir + "/vd_accounts",
	        .addresses = {"127.0.0.1"},
	        .bitrate_bps = 200'000'000,
	        .log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); },
	});
	session.start(&listener);
	auto ports = session.ports();

	// --- producer, the host pipeline's order of calls -----------------------------------------------------------
	std::atomic<bool> stop{false};
	std::atomic<int> render_ms{0};
	std::atomic<bool> noisy{false};
	std::atomic<bool> drift{false}; // submit with the next frame's id/display time, as the guest does when it ran ahead
	// drift mode: the pacer runs on its own thread like VrPipeline::frame_clock; the game takes the latest timing
	std::mutex clock_m;
	FrameTiming clock_t{};
	std::optional<TrackedPose> clock_head;
	bool clock_have = false;
	std::thread clock([&] {
		while (not stop)
		{
			if (not drift or not session.connected())
			{
				std::lock_guard l(clock_m);
				clock_have = false;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			FrameTiming t = session.next_frame_timing();
			auto head = session.pose_at(Device::head, t.predicted_display);
			std::lock_guard l(clock_m);
			clock_t = t, clock_head = head, clock_have = true;
		}
	}); // incompressible pictures (a game scene's worth of bits) instead of the pattern
	std::atomic<int64_t> max_submit_ns{0};
	std::atomic<int> submitted{0};
	std::thread producer([&] {
		TestPattern pattern;
		std::array<CVPixelBufferRef, 2> bufs{};
		uint32_t bw = 0;
		while (not stop)
		{
			auto info = session.headset();
			if (not session.connected() or not info)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			if (bw != info->eye_width)
			{
				for (auto & b: bufs)
				{
					if (b)
						CVPixelBufferRelease(b);
					b = make_nv12(info->eye_width, info->eye_height);
				}
				bw = info->eye_width;
			}
			FrameTiming t;
			std::optional<TrackedPose> head;
			if (drift)
			{
				std::lock_guard l(clock_m);
				if (not clock_have)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					continue;
				}
				t = clock_t, head = clock_head;
			}
			else
			{
				t = session.next_frame_timing();
				head = session.pose_at(Device::head, t.predicted_display); // VrPipeline::frame_clock
			}
			Frame f;
			f.frame_id = t.frame_id;
			f.display_time = t.predicted_display;
			for (int i = 0; i < 2; ++i)
			{
				f.pose[i] = head ? head->pose : Pose{};
				f.fov[i] = info->fov[i];
			}
			if (drift)
			{
				f.frame_id += 1;
				f.display_time += t.display_period * t.render_divisor;
			}
			if (int r = render_ms)
				std::this_thread::sleep_for(std::chrono::milliseconds(r)); // the game renders
			for (int i = 0; i < 2; ++i)
			{
				if (noisy)
				{
					CVPixelBufferLockBaseAddress(bufs[i], 0);
					for (int pl = 0; pl < 2; pl++)
					{
						auto * d = (uint32_t *)CVPixelBufferGetBaseAddressOfPlane(bufs[i], pl);
						size_t n = CVPixelBufferGetBytesPerRowOfPlane(bufs[i], pl) * CVPixelBufferGetHeightOfPlane(bufs[i], pl) / 4;
						for (size_t k = 0; k < n; k++)
							d[k] = arc4random() & 0x3f3f3f3f;
					}
					CVPixelBufferUnlockBaseAddress(bufs[i], 0);
				}
				else
					pattern.render(bufs[i], f.pose[i], f.fov[i], f.frame_id);
				f.eye[i].surface = CVPixelBufferGetIOSurface(bufs[i]);
			}
			int64_t t0 = now_ns();
			session.submit(f);
			int64_t dt = now_ns() - t0;
			for (int64_t m = max_submit_ns; dt > m and not max_submit_ns.compare_exchange_weak(m, dt);)
				;
			submitted++;
		}
		for (auto b: bufs)
			if (b)
				CVPixelBufferRelease(b);
	});

	FakeVdOptions base{.discovery_port = ports.discovery,
	                   .control_port = ports.control,
	                   .data_port = ports.data,
	                   .video_port = ports.video,
	                   .audio_port = ports.audio,
	                   .discovery_key = disc,
	                   .account = "owner"};
	base.user_settings = R"({"VRFramerate":72,"VRGraphicsQuality":0})";
	base.audio = false;
	base.tracking_period_ms = 2;
	base.tracking_fn = sample;

	// --- 1. world lock: the frame names the tracking sample it was rendered with --------------------------------
	{
		render_ms = 40; // a game slower than the display (VRChat loading: ~30 fps)
		FakeVdOptions o = base;
		o.frames = 20;
		auto r = run_fake_vd_client(o);
		if (not r.error.empty())
			fprintf(stderr, "world lock: %s\n", r.error.c_str());
		CHECK(r.error.empty());
		float worst = 0;
		int counted = 0;
		for (auto & md: r.frames)
		{
			if (md.frame_index <= 0)
				continue;
			float err = yaw_err(md.pose.eye[0].orientation, md.frame_index);
			worst = std::max(worst, err);
			counted++;
		}
		printf("world lock: %d frames, worst yaw error vs the named tracking sample %.4f rad (%.1f samples)\n", counted,
		       worst, worst / yaw_step);
		CHECK(counted >= 15);
		// exact: the frame is rendered with the named sample as the headset sent it. Before c54d61a the frame named the
		// sample current at submit (~20 samples newer); c54d61a still rendered a pose extrapolated 30 ms ahead with the
		// angular velocity (~15 samples), which the headset showed as stretching on head turns.
		CHECK(worst <= 0.25f * yaw_step);
		// FOV round trip: the headset's FovLeft/FovRight tangents come back unchanged in the frame (VD's FovPort:
		// up, down, left, right tangents, all positive), per eye, not mirrored or swapped
		bool fov_ok = not r.frames.empty();
		for (auto & md: r.frames)
		{
			auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
			auto & l = md.pose.fov[0];
			auto & rr = md.pose.fov[1];
			fov_ok = fov_ok and near(l.up, 1.0f) and near(l.down, 1.2f) and near(l.left, 1.3f) and near(l.right, 0.9f) and
			         near(rr.up, 1.0f) and near(rr.down, 1.2f) and near(rr.left, 0.9f) and near(rr.right, 1.3f);
		}
		CHECK(fov_ok);
	}

	// --- 1b. the same with the guest's frame id / display time one frame ahead of the pose it rendered -----------
	{
		render_ms = 40;
		drift = true;
		FakeVdOptions o = base;
		o.frames = 20;
		auto r = run_fake_vd_client(o);
		drift = false;
		CHECK(r.error.empty());
		float worst = 0;
		// from the second frame on: the first one may still carry the previous session's pose (the producer rendered it
		// before this headset's first sample arrived; nothing of this session matches it)
		for (size_t k = 1; k < r.frames.size(); ++k)
			if (r.frames[k].frame_index > 0)
				worst = std::max(worst, yaw_err(r.frames[k].pose.eye[0].orientation, r.frames[k].frame_index));
		printf("world lock, guest ahead: worst yaw error %.4f rad (%.1f samples)\n", worst, worst / yaw_step);
		CHECK(worst <= 0.25f * yaw_step); // matched by the echoed orientation, not by id/time
	}

	// --- 2. spacewarp toggled by the headset while frames flow ---------------------------------------------------
	{
		render_ms = 0;
		{
			std::lock_guard l(listener.m);
			listener.divisors.clear();
		}
		FakeVdOptions o = base;
		o.frames = 60;
		for (int i = 0; i < 8; ++i)
			o.mobile_schedule.push_back({4 + 5 * i, "UseSpacewarp", i % 2 ? "false" : "true"});
		auto r = run_fake_vd_client(o);
		if (not r.error.empty())
			fprintf(stderr, "spacewarp: %s\n", r.error.c_str());
		CHECK(r.error.empty());
		std::set<int> frame_times;
		for (auto & md: r.frames)
			frame_times.insert(int(std::lround(md.frame_time_ms * 10)));
		printf("spacewarp: frames %zu decoded %d/%d idr %d frame times(0.1 ms):", r.frames.size(), r.decoded[0],
		       r.decoded[1], r.idr[0]);
		for (int t: frame_times)
			printf(" %d", t);
		printf("\n");
		CHECK(r.frames.size() >= 60 and r.framing_errors == 0);
		CHECK(r.decoded[0] >= 58 and r.decoded[1] >= 58);
		CHECK(frame_times.contains(139) and frame_times.contains(278)); // 72 Hz, divisor 1 and 2
		CHECK(r.idr[0] >= 4); // each rate change restarts the encoders with a key frame
		std::lock_guard l(listener.m);
		CHECK(listener.divisors.size() == 8);
	}

	// --- 3. the headset stops reading video for 3 s ---------------------------------------------------------------
	{
		render_ms = 0;
		max_submit_ns = 0;
		FakeVdOptions o = base;
		o.frames = 40;
		o.stall_after = 10;
		o.stall_ms = 3000;
		o.timeout_ms = 30000;
		int before = submitted;
		auto r = run_fake_vd_client(o);
		int during = submitted - before;
		if (not r.error.empty())
			fprintf(stderr, "stall: %s\n", r.error.c_str());
		CHECK(r.error.empty());
		int after_stall = 0;
		for (int i: r.decoded_at)
			after_stall += i >= 15;
		printf("stall: producer submitted %d frames while the client ran, longest submit %.1f ms, dropped %llu, "
		       "frames received %zu, decoded after the stall %d\n",
		       during, max_submit_ns / 1e6, (unsigned long long)session.stats().frames_dropped.load(), r.frames.size(),
		       after_stall);
		CHECK(max_submit_ns < 250'000'000); // never waits for the headset (encode only)
		CHECK(during >= 3 * 72 / 2);        // the producer kept its cadence through the stall
		CHECK(session.stats().frames_dropped > 0);
		CHECK(after_stall >= 20); // recovered: frames after the stall decode (key frame after the drop)
	}

	// --- 5. a link slower than the stream: latency stays bounded (the backlog must not pile up in socket buffers) --
	{
		render_ms = 0;
		std::mutex tm;
		std::map<int32_t, int64_t> sent_at;
		std::vector<double> ages;
		FakeVdOptions o = base;
		o.frames = 100000;
		o.timeout_ms = 10000;
		o.keep_results = false;
		o.video_rate_bps = 12e6; // far below the 200 Mbit/s target
		o.tracking_fn = [&](int32_t n) {
			std::lock_guard l(tm);
			sent_at[n] = now_ns();
			return sample(n);
		};
		o.on_frame = [&](const FrameMetadata & md, CVPixelBufferRef, CVPixelBufferRef, int64_t arrival) {
			std::lock_guard l(tm);
			if (auto it = sent_at.find(md.frame_index); it != sent_at.end() and arrival - o.timeout_ms * 0 > 0)
				ages.push_back((arrival - it->second) / 1e6);
		};
		run_fake_vd_client(o);
		std::vector<double> late(ages.begin() + std::min<size_t>(ages.size(), ages.size() / 2), ages.end());
		std::sort(late.begin(), late.end());
		double p50 = late.empty() ? 0 : late[late.size() / 2], p99 = late.empty() ? 0 : late[late.size() * 99 / 100];
		printf("slow link: %zu frames, second half sample -> arrival p50 %.0f ms p99 %.0f ms\n", ages.size(), p50, p99);
		CHECK(ages.size() >= 20);
		CHECK(p99 < 400); // unbounded kernel buffering grows to seconds here
	}

	// --- 6. frames skipped for a slow link do not grow the next frames (encoder rate control) -----------------------
	{
		render_ms = 0;
		noisy = true;
		std::vector<size_t> sizes;
		std::mutex sm;
		FakeVdOptions o = base;
		o.frames = 100000;
		o.timeout_ms = 8000;
		o.keep_results = false;
		o.video_rate_bps = 60e6; // ~1/3 of the 200 Mbit/s stream: most frames are skipped
		o.on_frame = [&](const FrameMetadata & md, CVPixelBufferRef, CVPixelBufferRef, int64_t) {
			std::lock_guard l(sm);
			sizes.push_back(size_t(md.data_size));
		};
		run_fake_vd_client(o);
		noisy = false;
		double mean = 0;
		for (size_t k = sizes.size() / 2; k < sizes.size(); ++k)
			mean += double(sizes[k]);
		mean /= std::max<size_t>(1, sizes.size() - sizes.size() / 2);
		// per eye and frame: bitrate / 2 eyes / frame rate (72 Hz, divisor 1)
		double target = 200e6 / 8 / 2 / 72;
		printf("skipping link: %zu frames, second half mean left-eye frame %.0f KB (target %.0f KB)\n", sizes.size(), mean / 1024,
		       target / 1024);
		CHECK(sizes.size() >= 20);
		CHECK(mean < 2.0 * target); // with display-time timestamps the skipped frames' bits land on the next one: x3-x7
	}

	// --- 4. corrupt controller poses are untracked ---------------------------------------------------------------
	{
		FakeVdOptions o = base;
		o.frames = 3;
		o.tracking_fn = [](int32_t n) {
			TrackingSample s = sample(n);
			s.controller[0].tracked = controller_tracked;
			s.controller[0].orientation = {0, 0, 0, 0}; // what a sleeping controller may carry
			s.controller[0].position = {NAN, 0, 0};
			s.controller[1].tracked = controller_tracked;
			s.controller[1].orientation = {0, 0, 0, 1};
			s.controller[1].position = {0.2f, 1.2f, -0.3f};
			return s;
		};
		auto r = run_fake_vd_client(o);
		CHECK(r.error.empty());
		auto right = session.pose_at(Device::right_grip, now_ns());
		std::lock_guard l(listener.m);
		CHECK(listener.left and not listener.left->orientation_valid and not listener.left->position_valid);
		CHECK(listener.left and std::isfinite(listener.left->pose.position[0]) and listener.left->pose.orientation[3] == 1);
		CHECK(not right or right->orientation_valid); // a good pose next to it stays tracked
	}

	stop = true;
	producer.join();
	clock.join();
	session.stop();
	std::filesystem::remove_all(dir);
	return test_result("vd_stream");
}
