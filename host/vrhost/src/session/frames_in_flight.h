// SPDX-License-Identifier: GPL-3.0-or-later
// Frames-in-flight limit of the frame clock (VrPipeline, --frames-in-flight N): the next frame is released to the guest
// only when at most N-1 of the frames already released still wait for their copy to the host, so a GPU-bound game
// cannot queue frames ahead of the GPU (each queued frame is a frame period of motion-to-photon latency; live VRChat
// at 36 fps: submit -> copied 50 ms). Two exceptions keep poses flowing: while the game submits nothing (loading,
// paused) the clock is not held, and one wait never exceeds max_hold_ns (a frame id the game skips costs one late
// frame, not a stall). The bound is in time, not display periods: a GPU-bound game's frame takes several periods
// (VRChat at 24 fps: 41 ms+), and a bound of two periods released every frame before its copy, leaving no limit.
// N = adaptive (--frames-in-flight auto): instead of a count, the next frame waits for AdaptiveRelease's release time
// (session/adaptive_release.h), under the same two exceptions.
#pragma once
#include "session/adaptive_release.h"
#include "session/clock.h"
#include "session/vr_session.h" // ns

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace vrhost
{
class FramesInFlight
{
public:
	static constexpr int idle_periods = 8;           // no frame copied for this long: not rendering, do not hold
	static constexpr ns max_hold_ns = 100'000'000;  // a GPU frame of up to 100 ms (10 fps) still counts as rendering
	static constexpr uint32_t adaptive = ~0u;        // --frames-in-flight auto

	explicit FramesInFlight(uint32_t n) : n(n) {}
	bool enabled() const { return n > 0; }
	bool is_adaptive() const { return n == adaptive; }

	// Frame clock, before releasing the frame after `issued` (0: none yet). False when the wait timed out.
	bool wait(uint64_t issued, ns period, ns now)
	{
		if (not n or not issued)
			return true;
		std::unique_lock l(m);
		if (now - last_ready_time >= std::max(idle_periods * period, 2 * max_hold_ns))
			return true;
		if (n == adaptive)
		{
			// until the release time, which a frame coming back in the meantime may move
			const ns bound = now + max_hold_ns;
			for (;;)
			{
				ns t = now_ns(), target = std::min(ar.release_time(issued), bound);
				if (stop or t >= target)
					return stop or t < bound or ar.release_time(issued) <= t;
				cv.wait_for(l, std::chrono::nanoseconds(target - t));
			}
		}
		return cv.wait_for(l, std::chrono::nanoseconds(std::max(2 * period, max_hold_ns)), [&] { return stop or last_ready_id + n > issued; });
	}

	// Frame clock: frame `id` went to the guest at `t`.
	void released(uint64_t id, ns t)
	{
		if (n != adaptive)
			return;
		std::lock_guard l(m);
		ar.released(id, t);
	}

	// Link thread: frame `id` was received at `t`; the game submitted it at `submitted` and its GPU work ended (the
	// copy for the host) at `copied`, both for adaptive (0 = unknown).
	void ready(uint64_t id, ns t, ns submitted = 0, ns copied = 0)
	{
		{
			std::lock_guard l(m);
			last_ready_id = std::max(last_ready_id, id);
			last_ready_time = t;
			if (n == adaptive and submitted and copied)
				ar.frame(id, submitted, copied);
		}
		cv.notify_one();
	}

	// adaptive: the current estimates, for the stats line
	struct Estimate
	{
		ns cpu, gpu;
		bool holding;
	};
	Estimate estimate()
	{
		std::lock_guard l(m);
		return {ar.cpu_ns(), ar.gpu_ns(), ar.gpu_bound()};
	}

	void shutdown()
	{
		{
			std::lock_guard l(m);
			stop = true;
		}
		cv.notify_all();
	}

private:
	const uint32_t n;
	std::mutex m;
	std::condition_variable cv;
	uint64_t last_ready_id = 0;
	ns last_ready_time = 0;
	bool stop = false;
	AdaptiveRelease ar;
};
} // namespace vrhost
