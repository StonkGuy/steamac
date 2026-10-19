// SPDX-License-Identifier: GPL-3.0-or-later
// What the host accepts from the vm side of the VR link (guest-controlled values): swapchain descriptions, eye
// rects, field of view and poses, and the foveation parameters computed from them.
#include "check.h"
#include "compositor/foveation_params.h"
#include "compositor/vrlink_validate.h"

#include <cmath>
#include <cstdlib>
#include <limits>

using namespace vrhost;

static vrl_swapchain chain(uint32_t w, uint32_t h, uint32_t fmt = 80, uint32_t images = 3)
{
	vrl_swapchain s{{VRL_SWAPCHAIN, sizeof s}, 1, images, w, h, fmt, 0};
	return s;
}

// WiVRn's invariant for the parameters: sum(n_source * count) == source size, sum(count) == output size
static void check_params(const std::vector<uint16_t> & p, uint32_t src, uint32_t out)
{
	REQUIRE(p.size() % 2 == 1);
	if (p.size() == 1)
	{
		CHECK(p[0] == src);
		return;
	}
	int n_ratio = int(p.size() - 1) / 2;
	uint64_t s = 0, o = 0;
	for (size_t i = 0; i < p.size(); i++)
	{
		s += uint64_t(std::abs(n_ratio - int(i)) + 1) * p[i];
		o += p[i];
	}
	CHECK(s == src);
	CHECK(o == out);
}

int main()
{
	// swapchain descriptions
	CHECK(vrl_swapchain_ok(chain(1836, 1924)));
	CHECK(vrl_swapchain_ok(chain(16384, 16384, 115, 4)));
	CHECK(not vrl_swapchain_ok(chain(0, 100)));
	CHECK(not vrl_swapchain_ok(chain(16385, 100)));
	CHECK(not vrl_swapchain_ok(chain(100, 100, 80, 0)));
	CHECK(not vrl_swapchain_ok(chain(100, 100, 80, VRL_MAX_IMAGES + 1)));
	CHECK(not vrl_swapchain_ok(chain(100, 100, 252))); // depth format: not an exporter format
	auto c0 = chain(100, 100);
	c0.id = 0;
	CHECK(not vrl_swapchain_ok(c0));

	// the images must match: a smaller surface or another element size would be read past its end
	auto c = chain(1000, 800);
	CHECK(vrl_surface_ok(c, 1000, 800, 4));
	CHECK(not vrl_surface_ok(c, 999, 800, 4));
	CHECK(not vrl_surface_ok(c, 1000, 800, 8));
	CHECK(not vrl_surface_ok(chain(1000, 800, 115), 1000, 800, 4));

	// eye rects: inside the image, no 32-bit wrap-around
	CHECK(vrl_eye_rect_ok(vrl_eye{1, 0, 0, 0, 1000, 800, 0, 0}, c));
	CHECK(vrl_eye_rect_ok(vrl_eye{1, 0, 500, 0, 500, 800, 0, 0}, c));
	CHECK(not vrl_eye_rect_ok(vrl_eye{1, 0, 501, 0, 500, 800, 0, 0}, c));
	CHECK(not vrl_eye_rect_ok(vrl_eye{1, 0, 0xfffffff0u, 0, 0x20, 800, 0, 0}, c));
	CHECK(not vrl_eye_rect_ok(vrl_eye{1, 0, 0, 0xffffff00u, 1000, 0x200, 0, 0}, c));
	CHECK(not vrl_eye_rect_ok(vrl_eye{1, 0, 0, 0, 0, 800, 0, 0}, c));

	// field of view: finite, ordered, inside +-90 degrees (otherwise the headset's FOV is used)
	const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
	float good[4] = {-0.94f, 0.79f, 0.84f, -0.91f};
	CHECK(vrl_fov_ok(good));
	float bad[][4] = {{nan, 0.79f, 0.84f, -0.91f}, {-0.94f, inf, 0.84f, -0.91f}, {0.79f, -0.94f, 0.84f, -0.91f},
	                  {-0.94f, 0.79f, -0.91f, 0.84f}, {-2.f, 0.79f, 0.84f, -0.91f}, {0, 0, 0, 0}};
	for (auto & f: bad)
		CHECK(not vrl_fov_ok(f));
	vrl_submit s{};
	s.pose[0][3] = s.pose[1][3] = 1;
	CHECK(vrl_poses_ok(s));
	s.pose[1][5] = nan;
	CHECK(not vrl_poses_ok(s));

	// foveation from the checked values keeps WiVRn's invariant
	EyeFov fov{good[0], good[1], good[2], good[3]};
	auto p = compute_foveation(fov, -0.032f, 1836, 1924, 918, 962);
	check_params(p.x, 1836, 918);
	check_params(p.y, 1924, 962);
	CHECK(p.x.size() > 1 and p.y.size() > 1);

	// sizes past the 16-bit wire counts (found by fuzzing: assert / out-of-bounds write in fill_param_2d) and
	// degenerate FOVs fall back to the identity parameter instead
	p = compute_foveation(fov, 0, 0x53535353u, 0x53535353u, 1000, 1000);
	CHECK(p.x.size() == 1 and p.y.size() == 1);
	for (auto & f: bad)
	{
		p = compute_foveation(EyeFov{f[0], f[1], f[2], f[3]}, nan, 1836, 1924, 918, 962);
		check_params(p.x, 1836, 918);
		check_params(p.y, 1924, 962);
	}
	p = compute_foveation(fov, 0, 1836, 1924, 0, 0);
	CHECK(p.x.size() == 1 and p.y.size() == 1);
	return test_result("vrlink_validate");
}
