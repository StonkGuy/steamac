// SPDX-License-Identifier: GPL-3.0-or-later
// AdaptiveRelease (--frames-in-flight auto), against a simulated game in virtual time, next to the two fixed policies:
// no limit (0) and one frame in flight (1). The game is a D3D11 game under DXMT's export latency bound: woken by
// WaitGetPoses (the clock's timing), CPU work, submit, desktop present, then blocked until the frame before the one
// just presented has finished on the GPU; the GPU runs frames in order, each from the later of its submit and the end
// of the previous one. The clock holds the timing as the policy says, then the pacer (VdSession's grid: restarted after
// a longer hold) hands it out. Measured after a warm-up: frame rate, and latency from the timing's pose sample to the
// frame's GPU end (encode, network and decode come on top, the same in every policy).
//
// What auto must do: the frame rate of no limit with about the latency of one frame in flight when GPU-bound (each
// fixed policy fails one of the two there, which the test also shows), and no worse than no limit when the GPU is not
// the bottleneck (CPU-bound, light, changing load, jitter, hitches).
#include "check.h"

#include "session/adaptive_release.h"
#include "session/frames_in_flight.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <vector>

using namespace vrhost;

namespace
{
constexpr ns ms = 1'000'000;
constexpr ns present_ns = 1 * ms; // desktop mirror present after the submit

enum class Policy
{
	off,
	one,
	adaptive
};

struct Load
{
	ns cpu, gpu;
	double jitter = 0; // each frame's CPU and GPU time drawn uniformly in [1 - j, 1 + j] of the nominal
};

struct Result
{
	double fps, latency_ms;
};

// Time-stepped (0.05 ms): the clock and the game run side by side. The clock releases timings (ids 1, 2, ...): with
// no limit on every pacer tick; with a policy once it allows the timing after the last one (one in flight: when that
// one is back; auto: at its release time), then on the pacer's grid. The game takes the newest timing it has not used
// when it asks (and waits for the next if there is none), so timings it never asked for are skipped, as in the guest.
Result run(Policy policy, const std::function<Load(int)> & load, ns period, int frames = 600, int warmup = 60)
{
	constexpr ns dt = 50'000;
	std::mt19937 rng(7);
	AdaptiveRelease ar;
	std::vector<ns> rel_t{0};       // release time per timing id (index = id)
	uint64_t ready_id = 0;          // newest timing id whose frame came back (FramesInFlight's last_ready_id)
	ns next_tick = 0, hold_start = -1;
	struct Frame
	{
		uint64_t id = 0;
		ns pose = 0, submit = 0, done = 0;
	};
	std::vector<Frame> f;           // the game's frames, in order
	size_t delivered = 0;
	enum { asking, cpu, presenting } phase = asking;
	ns phase_end = 0, gpu_free = 0;
	uint64_t used = 0;
	for (ns t = 0; f.size() < size_t(frames) or delivered < f.size(); t += dt)
	{
		// frames whose GPU work ended come back to the clock, in order
		while (delivered < f.size() and f[delivered].done and f[delivered].done <= t)
		{
			const Frame & x = f[delivered++];
			ar.frame(x.id, x.submit, x.done);
			ready_id = std::max(ready_id, x.id);
		}
		// the clock
		uint64_t issued = rel_t.size() - 1;
		bool may = true;
		if (policy == Policy::one)
			may = not issued or ready_id >= issued;
		else if (policy == Policy::adaptive)
			may = not issued or ar.release_time(issued) <= t;
		if (hold_start < 0)
			hold_start = t;
		if (not may and t - hold_start >= FramesInFlight::max_hold_ns)
			may = true;
		if (may)
		{
			// VdSession::next_frame_timing: on the grid, restarted when the clock comes more than a period late
			if (next_tick == 0 or t - next_tick > period)
				next_tick = t;
			if (t >= next_tick)
			{
				rel_t.push_back(t);
				ar.released(rel_t.size() - 1, t);
				next_tick += period;
				hold_start = -1;
			}
		}
		// the game
		if (phase == asking and f.size() < size_t(frames) and rel_t.size() - 1 > used)
		{
			used = rel_t.size() - 1;
			Load l = load(int(f.size()) + 1);
			std::uniform_real_distribution<double> u(1 - l.jitter, 1 + l.jitter);
			ns c = ns(l.cpu * u(rng));
			f.push_back({used, rel_t[used], t + c, 0});
			f.back().done = std::max(t + c, gpu_free) + ns(l.gpu * u(rng));
			gpu_free = f.back().done;
			phase = cpu;
			phase_end = t + c;
		}
		else if (phase == cpu and t >= phase_end)
		{
			// submitted: present, then DXMT's bound: blocked until the frame before this one is done
			phase = presenting;
			phase_end = std::max(t + present_ns, f.size() >= 2 ? f[f.size() - 2].done : 0);
		}
		else if (phase == presenting and t >= phase_end)
			phase = asking;
	}
	std::vector<double> lat;
	for (size_t k = warmup; k < f.size(); k++)
		lat.push_back((f[k].done - f[k].pose) / 1e6);
	std::sort(lat.begin(), lat.end());
	return {(f.size() - 1 - warmup) / ((f.back().done - f[warmup].done) / 1e9), lat[lat.size() / 2]};
}

struct Trio
{
	Result off, one, adaptive;
};

Trio all(const char * name, const std::function<Load(int)> & load, ns period)
{
	Trio r{run(Policy::off, load, period), run(Policy::one, load, period), run(Policy::adaptive, load, period)};
	printf("%-28s off %5.1f fps %5.1f ms | one in flight %5.1f fps %5.1f ms | auto %5.1f fps %5.1f ms\n", name, r.off.fps,
	       r.off.latency_ms, r.one.fps, r.one.latency_ms, r.adaptive.fps, r.adaptive.latency_ms);
	return r;
}

Load constant(ns cpu, ns gpu, double jitter = 0)
{
	return {cpu, gpu, jitter};
}
} // namespace

int main()
{
	const ns p72 = 1'000'000'000 / 72;
	{
		// GPU-bound (VRChat on an M2: ~10 ms CPU, ~40 ms GPU): no limit queues behind the GPU, one in flight idles it
		Trio r = all("GPU-bound 10/40 ms", [](int) { return constant(10 * ms, 40 * ms); }, p72);
		CHECK(r.off.latency_ms > r.one.latency_ms + 20); // the problem: the pose waits a GPU frame
		CHECK(r.one.fps < 0.9 * r.off.fps);              // the fixed limit's cost
		CHECK(r.adaptive.fps >= 0.97 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.one.latency_ms + 5);
	}
	{
		// the same with frame-to-frame jitter of +-25 %
		Trio r = all("GPU-bound, jitter 25 %", [](int) { return constant(10 * ms, 40 * ms, 0.25); }, p72);
		CHECK(r.adaptive.fps >= 0.93 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.off.latency_ms - 15);
	}
	{
		// heavier CPU share, still GPU-bound
		Trio r = all("GPU-bound 25/40 ms", [](int) { return constant(25 * ms, 40 * ms); }, p72);
		CHECK(r.adaptive.fps >= 0.97 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.one.latency_ms + 5);
	}
	{
		// CPU-bound: nothing to gain, nothing may be lost
		Trio r = all("CPU-bound 35/15 ms", [](int) { return constant(35 * ms, 15 * ms); }, p72);
		CHECK(r.adaptive.fps >= 0.97 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.off.latency_ms + 2);
	}
	{
		// light game at the display rate
		Trio r = all("light 5/8 ms", [](int) { return constant(5 * ms, 8 * ms); }, p72);
		CHECK(r.adaptive.fps >= 0.97 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.off.latency_ms + 2);
	}
	{
		// the load changes: GPU-bound, then CPU-bound, then GPU-bound again (a world change)
		Trio r = all("GPU -> CPU -> GPU", [](int k) { return k > 200 and k <= 400 ? constant(35 * ms, 15 * ms) : constant(10 * ms, 40 * ms); }, p72);
		CHECK(r.adaptive.fps >= 0.95 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.off.latency_ms - 10);
	}
	{
		// a hitch every 50 frames (a 150 ms CPU frame, e.g. a garbage collection)
		Trio r = all("GPU-bound + CPU hitches", [](int k) { return k % 50 ? constant(10 * ms, 40 * ms) : constant(150 * ms, 40 * ms); }, p72);
		CHECK(r.adaptive.fps >= 0.95 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.one.latency_ms + 5);
	}
	{
		// 120 Hz headset, GPU-bound
		Trio r = all("GPU-bound 10/40 ms, 120 Hz", [](int) { return constant(10 * ms, 40 * ms); }, 1'000'000'000 / 120);
		CHECK(r.adaptive.fps >= 0.97 * r.off.fps);
		CHECK(r.adaptive.latency_ms <= r.one.latency_ms + 5);
	}
	{
		// no estimate before frames come back: the first frames are not held
		AdaptiveRelease ar;
		ar.released(1, 0);
		CHECK(ar.release_time(1) <= 0);
		CHECK(not ar.gpu_bound());
	}
	return test_result("adaptive_release");
}
