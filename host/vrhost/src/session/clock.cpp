// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/clock.h"
#include <time.h>

int64_t vrhost::now_ns()
{
	return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}
