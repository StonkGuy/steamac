// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal stand-in for Monado's os/os_time.h + util/u_time.h. The monotonic clock is steamac-vrhost's host clock
// (session/clock.h, CLOCK_UPTIME_RAW), so the pacer's times are in the same clock as every VrSession time.
#pragma once
#include <cassert>
#include <cstdint>
#include <time.h>
#define U_TIME_1MS_IN_NS 1000000
static inline int64_t os_monotonic_get_ns()
{
	return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}
