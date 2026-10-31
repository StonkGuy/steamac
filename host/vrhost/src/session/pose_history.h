// SPDX-License-Identifier: GPL-3.0-or-later
// Per-device pose history with interpolation and bounded extrapolation (role of WiVRn's driver/pose_list,
// rewritten without Monado types).
#pragma once
#include "session/vr_session.h"
#include <deque>
#include <mutex>

namespace vrhost
{
class PoseHistory
{
public:
	static constexpr ns max_extrapolation = 100'000'000; // like WiVRn's max_extrapolation_ns
	static constexpr size_t max_samples = 64;

	void add(const TrackedPose &);
	std::optional<TrackedPose> at(ns t) const;
	void clear();

private:
	mutable std::mutex mutex;
	std::deque<TrackedPose> samples; // sorted by time
};

Pose multiply(const Pose & a, const Pose & b); // a * b: b expressed in a's frame
std::array<float, 4> slerp(std::array<float, 4> a, std::array<float, 4> b, float t);
std::array<float, 3> rotate(const std::array<float, 4> & q, const std::array<float, 3> & v);
} // namespace vrhost
