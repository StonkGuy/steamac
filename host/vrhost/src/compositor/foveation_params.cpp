// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from WiVRn v26.9 server/compositor/foveation.cpp (GPL-3.0-or-later, Copyright (C) 2026 Patrick
// Nicolas): solve_foveation, defoveate, fill_param_2d, angles_to_center, convergence_angle, fill_ubo and the
// no-gaze branch of foveation::compute_params. Logic unchanged; xrt_* types replaced.
#include "foveation_params.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <tuple>

namespace vrhost
{
namespace
{
double defoveate(double a, double b, double l, double c, double x)
{
	return l / a * tan(a * x + b) + c;
}

std::tuple<float, float> solve_foveation(float l, float c)
{
	auto b = [l, c](double a) { return atan(a * (1 - c) / l) - a; };
	auto eq = [l, c](double a) { return atan(a * (1 - c) / l) + atan(a * (1 + c) / l) - 2 * a; };
	double a0 = 0;
	double a1 = 1;
	while (eq(a1) > 0)
		a1 *= 2;
	std::optional<double> f_a0;
	double f_a1 = eq(a1);
	int n = 0;
	double a = 0;
	while (std::abs(a1 - a0) > 0.0000001 && n++ < 100)
	{
		if (!f_a0)
		{
			a = 0.5 * (a0 + a1);
			double val = eq(a);
			if (val > 0)
			{
				a0 = a;
				f_a0 = val;
			}
			else
			{
				a1 = a;
				f_a1 = val;
			}
		}
		else
		{
			a = a1 - f_a1 * (a1 - a0) / (f_a1 - *f_a0);
			a0 = a1;
			a1 = a;
			f_a0 = f_a1;
			f_a1 = eq(a);
		}
	}
	return {a, b(a)};
}

float angles_to_center(float e, float l, float r)
{
	e = tan(e);
	l = tan(l);
	r = tan(r);
	float res = std::clamp((e - l) / (r - l) * 2 - 1, -1.f, 1.f);
	if (std::isnan(res))
		return 0;
	return res;
}

float convergence_angle(float distance, float eye_x, float gaze_yaw)
{
	float target_x = distance * std::sin(gaze_yaw);
	float target_z = distance * std::cos(gaze_yaw);
	return std::atan2(target_x - eye_x, target_z);
}

// false (out untouched) when the parameters do not form a strictly increasing source mapping
bool fill_param_2d(float c, size_t foveated_dim, size_t source_dim, std::vector<uint16_t> & out)
{
	float scale = float(foveated_dim) / source_dim;
	auto [a, b] = solve_foveation(scale, c);

	uint16_t last = 0;
	std::vector<uint16_t> left;
	std::vector<uint16_t> right;
	for (size_t i = 1; i < foveated_dim; ++i)
	{
		double u = (i * 2.) / foveated_dim - 1;
		auto f = defoveate(a, b, scale, c, u);
		double v = (f * 0.5 + 0.5) * source_dim + 0.5;
		if (not(v > last) or v >= double(source_dim))
			return false; // not strictly increasing (NaN included): the caller falls back to no foveation
		uint16_t n = uint16_t(v);
		if (n <= last)
			return false;
		size_t count = n - last;
		auto & vec = u < c ? left : right;
		if (count > vec.size())
			vec.resize(count);
		vec[count - 1]++;
		last = n;
	}
	size_t count = source_dim - last;
	if (count > right.size())
		right.resize(count);
	right[count - 1]++;

	count = std::max(left.size(), right.size());
	out.clear();
	out.resize(count - left.size());
	out.insert(out.end(), left.rbegin(), left.rend());
	if (!right.empty())
		out.back() += right.front();
	if (right.size() > 1)
		out.insert(out.end(), right.begin() + 1, right.end());
	out.resize(count * 2 - 1);
	return true;
}
} // namespace

FoveationParams compute_foveation(const EyeFov & fov, float eye_x, uint32_t src_w, uint32_t src_h, uint32_t out_w,
                                  uint32_t out_h)
{
	// WiVRn defaults without eye gaze: convergence at 1 m straight ahead, natural sight line 10 degrees down
	const float convergence_distance = 1;
	const float angle_offset = 10 * M_PI / 180;
	FoveationParams p;
	// The eye rect comes from the guest's submit (vrlink_server checks it against the image): sizes past the
	// wire format's 16-bit counts, or foveation that does not solve, fall back to the identity parameter.
	bool big = src_w > max_foveation_source or src_h > max_foveation_source;
	src_w = std::min(src_w, max_foveation_source);
	src_h = std::min(src_h, max_foveation_source);
	float angle_x = convergence_angle(convergence_distance, eye_x, 0);
	if (big or out_w == 0 or out_w >= src_w or not fill_param_2d(angles_to_center(angle_x, fov.left, fov.right), out_w, src_w, p.x))
		p.x = {uint16_t(src_w)};
	float angle_y = angle_offset;
	if (big or out_h == 0 or out_h >= src_h or not fill_param_2d(angles_to_center(-angle_y, fov.up, fov.down), out_h, src_h, p.y))
		p.y = {uint16_t(src_h)};
	return p;
}

std::vector<uint32_t> foveation_boundaries(const std::vector<uint16_t> & params, bool flip, uint32_t offset,
                                           uint32_t size, uint32_t out_dim)
{
	std::vector<uint32_t> ubo(out_dim + 1);
	assert(params.size() % 2 == 1);
	const int n_ratio = (params.size() - 1) / 2;
	size_t k = 0;
	ubo[0] = offset + (flip ? size : 0);
	int count = out_dim;
	for (size_t i = 0; i < params.size(); ++i)
	{
		// identity parameter {size}: one source pixel per output pixel
		const int n_source = params.size() == 1 ? 1 : std::abs(n_ratio - int(i)) + 1;
		for (size_t j = 0; j < params[i] && count > 0 && k + 1 < ubo.size(); ++j, --count, ++k)
			ubo[k + 1] = flip ? ubo[k] - n_source : ubo[k] + n_source;
	}
	for (++k; k < ubo.size(); ++k)
		ubo[k] = ubo[k - 1];
	return ubo;
}
} // namespace vrhost
