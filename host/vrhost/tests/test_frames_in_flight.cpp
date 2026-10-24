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
		// copies arriving out of order keep the newest id
		FramesInFlight g(1);
		g.ready(6, now_ns());
		g.ready(5, now_ns());
		ns t0 = now_ns();
		CHECK(g.wait(6, period, now_ns()));
		CHECK(ms_since(t0) < 40);
	}
	{
		// a copy that never comes (frame dropped): released after two periods, reported as a timeout
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
	return test_result("frames_in_flight");
}
