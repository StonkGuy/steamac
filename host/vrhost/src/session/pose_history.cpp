// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/pose_history.h"
#include <algorithm>
#include <cmath>

namespace vrhost
{
static std::array<float, 4> qmul(const std::array<float, 4> & a, const std::array<float, 4> & b)
{
	return {
	        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
}

static std::array<float, 4> normalize(std::array<float, 4> q)
{
	float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n == 0)
		return {0, 0, 0, 1};
	for (auto & c: q)
		c /= n;
	return q;
}

std::array<float, 3> rotate(const std::array<float, 4> & q, const std::array<float, 3> & v)
{
	auto r = qmul(qmul(q, {v[0], v[1], v[2], 0}), {-q[0], -q[1], -q[2], q[3]});
	return {r[0], r[1], r[2]};
}

Pose multiply(const Pose & a, const Pose & b)
{
	Pose r;
	r.orientation = normalize(qmul(a.orientation, b.orientation));
	auto p = rotate(a.orientation, b.position);
	for (int i = 0; i < 3; ++i)
		r.position[i] = a.position[i] + p[i];
	return r;
}

std::array<float, 4> slerp(std::array<float, 4> a, std::array<float, 4> b, float t)
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (d < 0)
	{
		for (auto & c: b)
			c = -c;
		d = -d;
	}
	if (d > 0.9995f)
	{
		std::array<float, 4> r;
		for (int i = 0; i < 4; ++i)
			r[i] = a[i] + t * (b[i] - a[i]);
		return normalize(r);
	}
	float theta = std::acos(d);
	float s = std::sin(theta);
	float wa = std::sin((1 - t) * theta) / s, wb = std::sin(t * theta) / s;
	std::array<float, 4> r;
	for (int i = 0; i < 4; ++i)
		r[i] = wa * a[i] + wb * b[i];
	return normalize(r);
}

// Integrate angular velocity (rad/s, world frame) for dt seconds.
static std::array<float, 4> integrate(const std::array<float, 4> & q, const std::array<float, 3> & w, float dt)
{
	float angle = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt;
	if (angle < 1e-9f)
		return q;
	float s = std::sin(angle / 2) / (angle / dt);
	std::array<float, 4> dq{w[0] * s, w[1] * s, w[2] * s, std::cos(angle / 2)};
	return normalize(qmul(dq, q));
}

void PoseHistory::add(const TrackedPose & p)
{
	std::lock_guard lock(mutex);
	auto it = std::upper_bound(samples.begin(), samples.end(), p.time, [](ns t, const TrackedPose & s) { return t < s.time; });
	if (it != samples.begin() and std::prev(it)->time == p.time)
		*std::prev(it) = p;
	else
		samples.insert(it, p);
	while (samples.size() > max_samples)
		samples.pop_front();
}

void PoseHistory::clear()
{
	std::lock_guard lock(mutex);
	samples.clear();
}

std::optional<TrackedPose> PoseHistory::at(ns t) const
{
	std::lock_guard lock(mutex);
	if (samples.empty())
		return std::nullopt;

	auto it = std::lower_bound(samples.begin(), samples.end(), t, [](const TrackedPose & s, ns t) { return s.time < t; });
	if (it != samples.end() and it->time == t)
		return *it;

	if (it != samples.begin() and it != samples.end())
	{
		const TrackedPose & a = *std::prev(it);
		const TrackedPose & b = *it;
		float f = float(t - a.time) / float(b.time - a.time);
		TrackedPose r = b;
		r.time = t;
		r.pose.orientation = slerp(a.pose.orientation, b.pose.orientation, f);
		for (int i = 0; i < 3; ++i)
		{
			r.pose.position[i] = a.pose.position[i] + f * (b.pose.position[i] - a.pose.position[i]);
			r.linear_velocity[i] = a.linear_velocity[i] + f * (b.linear_velocity[i] - a.linear_velocity[i]);
			r.angular_velocity[i] = a.angular_velocity[i] + f * (b.angular_velocity[i] - a.angular_velocity[i]);
		}
		return r;
	}

	// Before the oldest sample: hold it. After the newest: extrapolate with velocities, bounded.
	const TrackedPose & s = it == samples.begin() ? samples.front() : samples.back();
	TrackedPose r = s;
	r.time = t;
	if (t > s.time)
	{
		float dt = float(std::min<ns>(t - s.time, max_extrapolation)) * 1e-9f;
		r.pose.orientation = integrate(s.pose.orientation, s.angular_velocity, dt);
		for (int i = 0; i < 3; ++i)
			r.pose.position[i] += s.linear_velocity[i] * dt;
	}
	return r;
}
} // namespace vrhost
