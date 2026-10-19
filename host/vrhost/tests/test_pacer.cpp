// SPDX-License-Identifier: GPL-3.0-or-later
// WiVRn's pacer (server/compositor/pacer.cpp, vendored unmodified) driven by synthetic headset feedback in the
// headset clock: it must learn the decode time (99.5th percentile + 1 ms headset margin) and present that much
// earlier, learn blit-to-display, and phase-lock the predicted display times to the headset's vsync grid.
#include "check.h"

#include "compositor/pacer.h"
#include "driver/clock_offset.h"
#include "os/os_time.h"

#include <cmath>
#include <cstdio>
#include <thread>

static int64_t mod(int64_t a, int64_t m)
{
	return ((a % m) + m) % m;
}

int main()
{
	const int64_t period = 11'111'111; // 90 Hz
	const int64_t decode = 15'000'000;
	const int64_t phase = 4'321'000; // headset vsync phase, host clock
	const wivrn::clock_offset offset{.b = 5'000'000'000, .stable = true};

	wivrn::pacer pacer(period);
	CHECK(pacer.get_frame_duration() == uint64_t(period));

	int64_t id, wake, present, slop, display;
	pacer.predict(id, wake, present, slop, display);
	CHECK(id == 0);
	CHECK(display == present); // nothing learned yet: no decode margin, no blit-to-display time
	CHECK(wake - 3'000'000 < present); // wake-up includes the 3 ms the application is expected to wake early

	int64_t last_present_to_display = 0, last_display = 0, last_render_to_present = 0;
	for (int f = 1; f <= 600; ++f)
	{
		pacer.predict(id, wake, present, slop, display);
		CHECK(id == f);
		// what the headset reports for this frame (headset clock = host + b)
		int64_t decoded_host = present + decode;
		int64_t blit_host = (decoded_host - phase) / period * period + period + phase; // next vsync after decode
		wivrn::from_headset::feedback fb{};
		fb.frame_index = uint64_t(id);
		fb.stream_index = 0;
		fb.received_from_decoder = offset.to_headset(decoded_host);
		fb.blitted = offset.to_headset(blit_host);
		fb.displayed = fb.blitted + period;
		fb.times_displayed = 1;
		pacer.on_feedback(fb, offset);
		fb.stream_index = 1;
		fb.received_from_decoder += 500'000; // second eye decodes later: the frame counts as decoded then
		pacer.on_feedback(fb, offset);
		if (f % 100 == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(50)); // pacer's worker recomputes the percentile
		last_present_to_display = display - present;
		last_display = display;
	}
	pacer.predict(id, wake, present, slop, display);
	last_present_to_display = display - present;
	last_display = display;
	// present-to-display = safe decode (15.5 ms + 1 ms client margin) + blit-to-display (one period)
	double expect = (decode + 500'000 + 1'000'000 + period) / 1e6;
	printf("present->display %.3f ms (expect %.3f), display phase %.3f ms off the vsync grid, slop %.2f ms\n",
	       last_present_to_display / 1e6, expect, (mod(last_display - phase + period / 2, period) - period / 2) / 1e6, slop / 1e6);
	CHECK(std::abs(last_present_to_display / 1e6 - expect) < 0.3);
	CHECK(std::abs(mod(last_display - phase + period / 2, period) - period / 2) < 300'000); // phase-locked
	(void)last_render_to_present;

	// feedback for frames no longer in flight, repeated frames and unblitted frames is ignored
	wivrn::from_headset::feedback stale{};
	stale.frame_index = 3;
	stale.blitted = offset.to_headset(os_monotonic_get_ns());
	stale.received_from_decoder = stale.blitted + 1'000'000'000;
	pacer.on_feedback(stale, offset);
	int64_t id2, wake2, present2, slop2, display2;
	pacer.predict(id2, wake2, present2, slop2, display2);
	CHECK(std::abs((display2 - present2) - last_present_to_display) < 300'000);

	// refresh change
	pacer.set_frame_duration(13'888'889);
	CHECK(pacer.get_frame_duration() == 13'888'889u);
	return test_result("pacer");
}
