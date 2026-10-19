// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter (personal use): geometry of the picture as the Quest shows it. The VD client undoes VD's
// fixed foveated encoding on every frame (vd_foveation_source / _encoded), so the host must apply it. A world-fixed
// scene (lines every 15 degrees of azimuth and elevation, a thick horizon) is rendered for the Quest 3's asymmetric
// FOV as the game does, goes through EyeCompositor into the stream, and is then shown like the VD client does:
// each display pixel samples the decoded stream at vd_foveation_encoded(display coordinate). Every scene line must be
// where the scene puts it: static straightness, with the head rolled 90 degrees, and turned. Reported separately
// for the centre and the edges (centre magnification vs edge compression), and for the old path (no foveated
// encoding, the guest rendering at the stream size) which shows the 2026-10-19 "magnifying glass".
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "check.h"
#include "compositor/eye_compositor.h"
#include "compositor/foveation_params.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace vrhost;

namespace
{
// the Quest 3 under VD (2026-10-17..19 live logs): left eye FOV in radians; the right eye mirrors it
constexpr EyeFov kFov[2] = {{-0.94f, 0.70f, 0.77f, -0.96f}, {-0.70f, 0.94f, 0.77f, -0.96f}};
constexpr double kDeg = M_PI / 180;

struct Pose
{
	double roll = 0, yaw = 0, pitch = 0;
};

// world scene luma for a world direction: lines at multiples of 15 degrees (0.3 degrees wide), horizon 0.8 degrees
double scene(double x, double y, double z)
{
	double az = std::atan2(x, -z) / kDeg, el = std::asin(std::clamp(y, -1.0, 1.0)) / kDeg;
	auto near = [](double a, double w) {
		double m = std::fmod(std::fmod(a, 15.0) + 15.0, 15.0);
		return std::min(m, 15.0 - m) < w;
	};
	if (std::abs(el) < 0.4)
		return 1;
	return (near(az, 0.15) and std::abs(el) < 75) or near(el, 0.15) ? 1 : 0;
}

// world direction of a display/render coordinate (sx, sy in [0,1], y down) for an eye FOV and head pose
void ray(const EyeFov & f, const Pose & p, double sx, double sy, double & x, double & y, double & z)
{
	double tl = std::tan(f.left), tr = std::tan(f.right), tu = std::tan(f.up), td = std::tan(f.down);
	double hx = tl + sx * (tr - tl), hy = tu + sy * (td - tu), hz = -1;
	double n = std::sqrt(hx * hx + hy * hy + 1);
	hx /= n, hy /= n, hz /= n;
	// roll about -z (view axis), then pitch about x, then yaw about y
	double cr = std::cos(p.roll), sr = std::sin(p.roll);
	double x1 = cr * hx - sr * hy, y1 = sr * hx + cr * hy, z1 = hz;
	double cp = std::cos(p.pitch), sp = std::sin(p.pitch);
	double y2 = cp * y1 - sp * z1, z2 = sp * y1 + cp * z1, x2 = x1;
	double cy = std::cos(p.yaw), sy_ = std::sin(p.yaw);
	x = cy * x2 + sy_ * z2, z = -sy_ * x2 + cy * z2, y = y2;
}

IOSurfaceRef render_eye(uint32_t w, uint32_t h, const EyeFov & f, const Pose & p)
{
	NSDictionary * props = @{
		(id)kIOSurfaceWidth: @(w),
		(id)kIOSurfaceHeight: @(h),
		(id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfacePixelFormat: @(uint32_t('BGRA')),
	};
	IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	IOSurfaceLock(s, 0, nullptr);
	auto * base = (uint8_t *)IOSurfaceGetBaseAddress(s);
	size_t bpr = IOSurfaceGetBytesPerRow(s);
	for (uint32_t yy = 0; yy < h; yy++)
		for (uint32_t xx = 0; xx < w; xx++)
		{
			double x, y, z;
			ray(f, p, (xx + 0.5) / w, (yy + 0.5) / h, x, y, z);
			uint8_t v = scene(x, y, z) > 0.5 ? 255 : 0;
			uint8_t * px = base + yy * bpr + xx * 4;
			px[0] = px[1] = px[2] = v;
			px[3] = 255;
		}
	IOSurfaceUnlock(s, 0, nullptr);
	return s;
}

CVPixelBufferRef nv12(uint32_t w, uint32_t h)
{
	NSDictionary * attrs = @{
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
	};
	CVPixelBufferRef b = nullptr;
	CVPixelBufferCreate(nullptr, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, (__bridge CFDictionaryRef)attrs, &b);
	return b;
}

struct Errors
{
	double centre_p99 = 0, edge_p99 = 0, centre_max = 0, edge_max = 0; // degrees
	int lines = 0, missing = 0;
};

double pct(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

// Shows the decoded left eye like the VD client (display coordinate s -> stream coordinate vd_foveation_encoded(s))
// on a display grid of dw x dh and compares the line centres along rows and columns with the scene's own.
Errors measure(CVPixelBufferRef stream, uint32_t dw, uint32_t dh, const EyeFov & f, const Pose & p)
{
	CVPixelBufferLockBaseAddress(stream, kCVPixelBufferLock_ReadOnly);
	auto * luma = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(stream, 0);
	size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(stream, 0), sw = CVPixelBufferGetWidthOfPlane(stream, 0),
	       sh = CVPixelBufferGetHeightOfPlane(stream, 0);
	auto shown = [&](uint32_t x, uint32_t y) {
		double u = vd_foveation_encoded((x + 0.5) / dw), v = vd_foveation_encoded((y + 0.5) / dh);
		uint32_t px = std::min<uint32_t>(uint32_t(u * sw), sw - 1), py = std::min<uint32_t>(uint32_t(v * sh), sh - 1);
		return luma[py * bpr + px] > 128;
	};
	auto truth = [&](uint32_t x, uint32_t y) {
		double rx, ry, rz;
		ray(f, p, (x + 0.5) / dw, (y + 0.5) / dh, rx, ry, rz);
		return scene(rx, ry, rz) > 0.5;
	};
	// degrees per display pixel (average over the FOV, per axis)
	double deg_x = (f.right - f.left) / kDeg / dw, deg_y = (f.up - f.down) / kDeg / dh;
	std::vector<double> centre, edge;
	Errors e;
	auto scan = [&](bool rows) {
		uint32_t n = rows ? dh : dw, len = rows ? dw : dh;
		for (uint32_t fixed = n / 16; fixed < n; fixed += n / 16)
		{
			std::vector<double> a, b;
			auto runs = [&](auto get, std::vector<double> & out) {
				int start = -1;
				for (uint32_t i = 0; i <= len; i++)
				{
					bool on = i < len and get(i);
					if (on and start < 0)
						start = int(i);
					if (not on and start >= 0)
					{
						out.push_back((start + i - 1) / 2.0);
						start = -1;
					}
				}
			};
			runs([&](uint32_t i) { return rows ? truth(i, fixed) : truth(fixed, i); }, a);
			runs([&](uint32_t i) { return rows ? shown(i, fixed) : shown(fixed, i); }, b);
			for (double t: a)
			{
				double best = 1e9;
				for (double s: b)
					best = std::min(best, std::abs(s - t));
				e.lines++;
				double deg = best * (rows ? deg_x : deg_y);
				if (deg > 5)
				{
					e.missing++;
					deg = 5;
				}
				double c = t / len - 0.5;
				(std::abs(c) < 0.25 ? centre : edge).push_back(deg);
			}
		}
	};
	scan(true);
	scan(false);
	CVPixelBufferUnlockBaseAddress(stream, kCVPixelBufferLock_ReadOnly);
	e.centre_p99 = pct(centre, .99), e.edge_p99 = pct(edge, .99), e.centre_max = pct(centre, 1), e.edge_max = pct(edge, 1);
	return e;
}

Errors run(const char * label, bool vd_foveation, const Pose & p)
{
	const uint32_t out_w = 1152, out_h = 1216; // VD "Low" stream
	// with the encoding the guest renders at VD's render size (1.5x); the old path rendered at the stream size
	const uint32_t rw = vd_foveation ? 1728 : out_w, rh = vd_foveation ? 1824 : out_h;
	IOSurfaceRef surf[2];
	EyeSource src[2];
	for (int e = 0; e < 2; e++)
	{
		surf[e] = render_eye(rw, rh, kFov[e], p);
		src[e] = {surf[e], uint32_t(MTLPixelFormatBGRA8Unorm), false, 0, 0, rw, rh, false};
	}
	CompositorOptions opt;
	opt.vd_foveation = vd_foveation;
	EyeCompositor comp(out_w, out_h, opt);
	CVPixelBufferRef out[2] = {nv12(out_w, out_h), nv12(out_w, out_h)};
	FoveationParams params[2];
	float eye_x[2] = {-0.032f, 0.032f};
	bool ok = comp.compose(src, kFov, eye_x, out, params);
	CHECK(ok);
	if (getenv("VDFOV_IDEAL"))
	{
		// reference: an exact encoder (scene sampled at the encoded pixel's own direction), to size the metric's floor
		CVPixelBufferLockBaseAddress(out[0], 0);
		auto * l = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(out[0], 0);
		size_t bp = CVPixelBufferGetBytesPerRowOfPlane(out[0], 0);
		for (uint32_t yy = 0; yy < out_h; yy++)
			for (uint32_t xx = 0; xx < out_w; xx++)
			{
				double sx = vd_foveation ? vd_foveation_source((xx + 0.5) / out_w) : (xx + 0.5) / out_w;
				double sy = vd_foveation ? vd_foveation_source((yy + 0.5) / out_h) : (yy + 0.5) / out_h;
				double x, y, z;
				ray(kFov[0], p, sx, sy, x, y, z);
				l[yy * bp + xx] = scene(x, y, z) > 0.5 ? 255 : 0;
			}
		CVPixelBufferUnlockBaseAddress(out[0], 0);
	}
	Errors e = measure(out[0], 1728, 1824, kFov[0], p);
	printf("%-34s lines %4d (missing %3d) | centre p99 %.2f max %.2f deg | edges p99 %.2f max %.2f deg\n", label, e.lines,
	       e.missing, e.centre_p99, e.centre_max, e.edge_p99, e.edge_max);
	for (int k = 0; k < 2; k++)
	{
		CVPixelBufferRelease(out[k]);
		CFRelease(surf[k]);
	}
	return e;
}
} // namespace

int main()
{
	@autoreleasepool
	{
		// the curve itself: continuous, monotonic, inverse of each other, 1.5x in the centre
		double worst = 0;
		for (int i = 0; i <= 1000; i++)
		{
			double u = i / 1000.0;
			worst = std::max(worst, std::abs(vd_foveation_encoded(vd_foveation_source(u)) - u));
			if (i)
				CHECK(vd_foveation_source(u) > vd_foveation_source((i - 1) / 1000.0));
		}
		CHECK(worst < 1e-9);
		CHECK(std::abs(vd_foveation_source(0.125) - 0.25) < 1e-9 and std::abs(vd_foveation_source(0.875) - 0.75) < 1e-9);
		CHECK(std::abs((vd_foveation_source(0.6) - vd_foveation_source(0.4)) / 0.2 - 2.0 / 3.0) < 1e-9);
		// boundaries: monotonic, cover the source, flip mirrors
		auto b = vd_foveation_boundaries(false, 0, 1728, 1152);
		auto bf = vd_foveation_boundaries(true, 0, 1728, 1152);
		CHECK(b.front() == 0 and b.back() == 1728 and bf.front() == 1728 and bf.back() == 0);
		bool mono = true;
		for (size_t i = 1; i < b.size(); i++)
			mono = mono and b[i] > b[i - 1] and bf[i] < bf[i - 1];
		CHECK(mono);

		Pose still{}, rolled{.roll = 90 * kDeg}, turned{.roll = 20 * kDeg, .yaw = 35 * kDeg, .pitch = -15 * kDeg};
		Errors old_still = run("old path, head still", false, still);
		Errors old_roll = run("old path, head rolled 90 deg", false, rolled);
		Errors s = run("VD encoding, head still", true, still);
		Errors r = run("VD encoding, head rolled 90 deg", true, rolled);
		Errors t = run("VD encoding, turned/rolled/pitched", true, turned);
		// the old path must show the reported bend (it is what the headset displayed), the new one must not
		CHECK(old_still.edge_max > 2.0 and old_roll.edge_max > 2.0);
		for (const Errors * e: {&s, &r, &t})
		{
			CHECK(e->missing <= e->lines / 100);
			// metric floor (VDFOV_IDEAL=1, an exact point-sampling encoder): centre max 0.16-0.41, edges worse (thin lines
			// alias away where the periphery is stored at down to 1/4 resolution); the old path is off by > 5 degrees
			CHECK(e->centre_max <= 0.45);
			CHECK(e->edge_p99 <= 0.8);
		}
		return test_result("vd_foveation");
	}
}
