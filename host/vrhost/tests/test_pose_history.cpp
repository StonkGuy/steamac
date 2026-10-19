// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.h"
#include "session/pose_history.h"
#include <cmath>

using namespace vrhost;

static bool near(float a, float b, float eps = 1e-3f)
{
	return std::abs(a - b) < eps;
}

static Pose yaw(float a, float x = 0)
{
	return Pose{.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)}, .position = {x, 0, 0}};
}

int main()
{
	PoseHistory h;
	CHECK(not h.at(0));

	h.add({.device = Device::head, .time = 1000, .pose = yaw(0, 0), .orientation_valid = true});
	h.add({.device = Device::head, .time = 3000, .pose = yaw(1.0f, 2), .orientation_valid = true});

	// interpolation at the midpoint: yaw 0.5, x = 1
	auto m = h.at(2000);
	REQUIRE(m);
	CHECK(near(m->pose.orientation[1], std::sin(0.25f)));
	CHECK(near(m->pose.position[0], 1));

	// exact sample
	CHECK(near(h.at(3000)->pose.position[0], 2));
	// before the oldest: held
	CHECK(near(h.at(0)->pose.position[0], 0));

	// extrapolation with angular velocity 1 rad/s around +Y and linear 1 m/s on X, 50 ms ahead
	PoseHistory e;
	e.add({.device = Device::head, .time = 0, .pose = yaw(0), .linear_velocity = {1, 0, 0}, .angular_velocity = {0, 1, 0}, .orientation_valid = true});
	auto x = e.at(50'000'000);
	REQUIRE(x);
	CHECK(near(x->pose.position[0], 0.05f));
	CHECK(near(x->pose.orientation[1], std::sin(0.025f)));
	// bounded at max_extrapolation (100 ms)
	auto far = e.at(1'000'000'000);
	CHECK(near(far->pose.position[0], 0.1f));

	// multiply: head yawed 90 degrees, eye offset +X in head space -> world -Z
	Pose head = yaw(float(M_PI / 2));
	Pose eye{.position = {0.032f, 0, 0}};
	Pose w = multiply(head, eye);
	CHECK(near(w.position[0], 0));
	CHECK(near(w.position[2], -0.032f));

	// history keeps at most max_samples, sorted
	PoseHistory big;
	for (int i = 0; i < 200; ++i)
		big.add({.device = Device::head, .time = 200 - i, .pose = yaw(0, float(200 - i)), .orientation_valid = true});
	CHECK(near(big.at(200)->pose.position[0], 200));
	return test_result("pose_history");
}
