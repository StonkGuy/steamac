// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace vrhost
{
// Host monotonic clock in ns (CLOCK_UPTIME_RAW: same base as mach_absolute_time, CoreVideo and Metal timestamps).
int64_t now_ns();
} // namespace vrhost
