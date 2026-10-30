// SPDX-License-Identifier: GPL-3.0-or-later
// FramesInFlight (the frame clock's --frames-in-flight limit): the next frame is held until enough released frames are
// copied, released as soon as the copy arrives, never held while the game renders nothing, never longer than two frame
// periods, and released at shutdown.
#include "check.h"

#include "session/clock.h"
#include "session/frames_in_flight.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

using namespace vrhost;

static double ms_since(ns t0)
{
	return (now_ns() - t0) / 1e6;
}

int main()
{
	const ns period = 50'000'000; // wide enough that 'at once', 'held' and 'timed out' stay apart on a loaded machine

	{
		// off, and before the first frame: never holds
		FramesInFlight off(0);
		CHECK(not off.enabled());
		CHECK(off.wait(5, period, now_ns()));
		FramesInFlight on(2);
		CHECK(on.enabled());
		ns t0 = now_ns();
		CHECK(on.wait(0, period, now_ns()));
		CHECK(ms_since(t0) < 40);
	}
	{
		// N = 2: frame 5 issued, 4 copied -> frame 6 may start at once (CPU of 6 overlaps the GPU of 5)
		FramesInFlight g(2);
		g.ready(4, now_ns());
		ns t0 = now_ns();
		CHECK(g.wait(5, period, now_ns()));
		CHECK(ms_since(t0) < 40);
	}
	{
		// N = 2: only frame 3 copied -> held until the copy of 4 arrives, then released before the bound
		FramesInFlight g(2);
		g.ready(3, now_ns());
		ns t0 = now_ns();
		auto copy = std::async(std::launch::async, [&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			g.ready(4, now_ns());
		});
		CHECK(g.wait(5, period, now_ns()));
		double waited = ms_since(t0);
		CHECK(waited >= 19 and waited < 99);
		copy.get();
	}
	{
		// N = 1: the previous frame itself must be copied
		FramesInFlight g(1);
		g.ready(4, now_ns());
		auto copy = std::async(std::launch::async, [&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			g.ready(5, now_ns());
		});
		ns t0 = now_ns();
		CHECK(g.wait(5, period, now_ns()));
		CHECK(ms_since(t0) >= 19);
		copy.get();
	}
	{
		// a GPU-bound game: 100 Hz display, 60 ms GPU frames -> held until the copy, not released after two periods
		// (the bound is time, not display periods; with a two-period bound the limit never held a frame)
		FramesInFlight g(1);
		const ns fast = 10'000'000;
		g.ready(4, now_ns());
		auto copy = std::async(std::launch::async, [&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
			g.ready(5, now_ns());
		});
		ns t0 = now_ns();
		CHECK(g.wait(5, fast, now_ns()));
		double waited = ms_since(t0);
		CHECK(waited >= 59 and waited < FramesInFlight::max_hold_ns / 1e6);
		copy.get();
	}
	{
		// copies arriving out of order keep the newest id
		FramesInFlight g(1);
		g.ready(6, now_ns());
		g.ready(5, now_ns());
		ns t0 = now_ns();
		CHECK(g.wait(6, period, now_ns()));
		CHECK(ms_since(t0) < 40);
	}
	{
		// a copy that never comes (frame dropped): released after max(two periods, max_hold_ns), reported as a timeout
		FramesInFlight g(2);
		g.ready(3, now_ns());
		ns t0 = now_ns();
		CHECK(not g.wait(10, period, now_ns()));
		double waited = ms_since(t0);
		CHECK(waited >= 99 and waited < 1000);
	}
	{
		// the game rendered nothing for idle_periods (loading, paused): not held at all
		FramesInFlight g(2);
		ns now = now_ns();
		g.ready(3, now - FramesInFlight::idle_periods * period);
		ns t0 = now_ns();
		CHECK(g.wait(10, period, now));
		CHECK(ms_since(t0) < 40);
	}
	{
		// shutdown releases a held clock
		FramesInFlight g(2);
		g.ready(3, now_ns());
		auto stop = std::async(std::launch::async, [&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			g.shutdown();
		});
		ns t0 = now_ns();
		g.wait(10, 1'000'000'000, now_ns());
		CHECK(ms_since(t0) < 500);
		stop.get();
	}
	{
		// auto, no estimate yet (no frame came back): not held
		FramesInFlight g(FramesInFlight::adaptive);
		CHECK(g.enabled() and g.is_adaptive());
		g.released(1, now_ns());
		g.ready(1, now_ns());
		ns t0 = now_ns();
		CHECK(g.wait(2, period, now_ns()));
		CHECK(ms_since(t0) < 40);
	}
	{
		// auto, GPU-bound history (CPU 10 ms, GPU 40 ms, frames back to back): frame 6 went out 5 ms ago, so the GPU
		// should finish it in 45 ms and frame 7 goes out 10 ms + margin before that, about 33 ms from now
		FramesInFlight g(FramesInFlight::adaptive);
		const ns ms = 1'000'000, base = now_ns();
		for (uint64_t i = 1; i <= 5; i++)
		{
			ns copied = base - ns(5 - i) * 40 * ms, submit = copied - 40 * ms;
			g.released(i, submit - 10 * ms);
			g.ready(i, copied, submit, copied);
		}
		g.released(6, base - 5 * ms);
		auto e = g.estimate();
		CHECK(e.holding and e.cpu == 10 * ms and e.gpu == 40 * ms);
		CHECK(g.wait(6, period, base));
		double waited = ms_since(base);
		CHECK(waited >= 30 and waited < 60);
	}
	{
		// auto, frame 6 comes back early (its GPU work ended at once): released then, not at the predicted time
		FramesInFlight g(FramesInFlight::adaptive);
		const ns ms = 1'000'000, base = now_ns();
		for (uint64_t i = 1; i <= 5; i++)
		{
			ns copied = base - ns(5 - i) * 40 * ms, submit = copied - 40 * ms;
			g.released(i, submit - 10 * ms);
			g.ready(i, copied, submit, copied);
		}
		g.released(6, base - 5 * ms);
		auto back = std::async(std::launch::async, [&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			ns t = now_ns();
			g.ready(6, t, t - 1 * ms, t);
		});
		CHECK(g.wait(6, period, base));
		double waited = ms_since(base);
		CHECK(waited >= 4 and waited < 25);
		back.get();
	}
	{
		// auto, a GPU frame longer than the bound (150 ms): held for max_hold_ns, reported as a timeout
		FramesInFlight g(FramesInFlight::adaptive);
		const ns ms = 1'000'000, base = now_ns();
		for (uint64_t i = 1; i <= 5; i++)
		{
			ns copied = base - ns(5 - i) * 150 * ms, submit = copied - 150 * ms;
			g.released(i, submit - 10 * ms);
			g.ready(i, copied, submit, copied);
		}
		g.released(6, base);
		CHECK(not g.wait(6, period, base));
		double waited = ms_since(base);
		CHECK(waited >= 99 and waited < 300);
	}
	return test_result("frames_in_flight");
}
