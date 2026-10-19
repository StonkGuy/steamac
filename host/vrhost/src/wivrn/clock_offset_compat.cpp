// SPDX-License-Identifier: GPL-3.0-or-later
// wivrn::clock_offset conversions, copied from WiVRn v26.9 server/driver/clock_offset.cpp (the estimator there needs
// Monado and wivrn_connection; steamac-vrhost keeps its own estimator in wivrn_session.cpp and hands the pacer a
// wivrn::clock_offset built from it).
#include "driver/clock_offset.h"

namespace wivrn
{
XrTime clock_offset::from_headset(XrTime ts) const
{
	return ts - b;
}

XrTime clock_offset::to_headset(XrTime timestamp_ns) const
{
	return timestamp_ns + b;
}
} // namespace wivrn
