// SPDX-License-Identifier: GPL-3.0-or-later
// Frames-in-flight limit of the frame clock (VrPipeline, --frames-in-flight N): the next frame is released to the guest
// only when at most N-1 of the frames already released still wait for their copy to the host, so a GPU-bound game
// cannot queue frames ahead of the GPU (each queued frame is a frame period of motion-to-photon latency; live VRChat
// at 36 fps: submit -> copied 50 ms). Two exceptions keep poses flowing: while the game submits nothing (loading,
// paused) the clock is not held, and one wait never exceeds two frame periods (a frame id the game skips costs one late
// frame, not a stall).
#pragma once
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
	static constexpr int idle_periods = 8; // no frame copied for this long: the game is not rendering, do not hold

	explicit FramesInFlight(uint32_t n) : n(n) {}
	bool enabled() const { return n > 0; }

	// Frame clock, before releasing the frame after `issued` (0: none yet). False when the wait timed out.
	bool wait(uint64_t issued, ns period, ns now)
	{
		if (not n or not issued)
			return true;
		std::unique_lock l(m);
		if (now - last_ready_time >= idle_periods * period)
			return true;
		return cv.wait_for(l, std::chrono::nanoseconds(2 * period), [&] { return stop or last_ready_id + n > issued; });
	}

	// Link thread: frame `id` was copied to the host at `t`.
	void ready(uint64_t id, ns t)
	{
		{
			std::lock_guard l(m);
			last_ready_id = std::max(last_ready_id, id);
			last_ready_time = t;
		}
		cv.notify_one();
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
};
} // namespace vrhost
