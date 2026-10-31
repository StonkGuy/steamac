// SPDX-License-Identifier: GPL-3.0-or-later
// Geometry of the foveated stream as the headset sees it: a grid eye image at the Quest 3 render size goes through
// EyeCompositor into the stream size the Quest 3 WiVRn client asks for (half per axis), is defoveated with the WiVRn
// v26.9 client's algorithm (wivrn_defoveate.h) and every grid line must come back where it was in the game's image.
// A mismatch here is a squashed / stretched picture in the headset. Covers separate eye images, a double-wide image
// with per-eye rects and bottom-up rows; also checks that a turning head does not change the parameters (eye x).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "check.h"
#include "compositor/eye_compositor.h"
#include "compositor/vrlink_server.h"
#include "wivrn_defoveate.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

using namespace vrhost;

namespace
{
constexpr uint32_t kGrid = 64, kLine = 4; // white lines at multiples of 64 px, 4 px wide

IOSurfaceRef grid_surface(uint32_t w, uint32_t h, uint32_t eye_w, uint32_t grid = kGrid, uint32_t lw = kLine)
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
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			// grid relative to each eye's own origin (both halves of a double-wide image are eye-sized)
			bool line = ((x % eye_w) % grid) < lw or (y % grid) < lw;
			uint8_t v = line ? 255 : 0;
			uint8_t * p = base + y * bpr + x * 4;
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	IOSurfaceUnlock(s, 0, nullptr);
	return s;
}

CVPixelBufferPoolRef nv12_pool(uint32_t w, uint32_t h)
{
	NSDictionary * attrs = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
		(id)kCVPixelBufferWidthKey: @(w),
		(id)kCVPixelBufferHeightKey: @(h),
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
	};
	CVPixelBufferPoolRef pool = nullptr;
	CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attrs, &pool);
	return pool;
}

// centres of bright runs along a row (horizontal = true) or a column of the defoveated image
std::vector<double> line_centres(const test::Image & img, uint32_t fixed, bool horizontal)
{
	std::vector<double> c;
	uint32_t n = horizontal ? img.w : img.h;
	int start = -1;
	for (uint32_t i = 0; i <= n; i++)
	{
		bool bright = i < n and (horizontal ? img.luma(i, fixed) : img.luma(fixed, i)) > 90;
		if (bright and start < 0)
			start = int(i);
		if (not bright and start >= 0)
		{
			c.push_back((start + i - 1) / 2.0);
			start = -1;
		}
	}
	return c;
}

// every grid line of the game image must be found within tol px of its source position
void check_axis(const char * what, const test::Image & img, uint32_t fixed, bool horizontal, uint32_t src_dim, bool flip)
{
	auto found = line_centres(img, fixed, horizontal);
	int missing = 0;
	double worst = 0;
	for (uint32_t k = 0; k * kGrid < src_dim; k++)
	{
		double expect = k * kGrid + (kLine - 1) / 2.0;
		if (flip)
			expect = src_dim - 1 - expect;
		double best = 1e9;
		for (double f: found)
			best = std::min(best, std::abs(f - expect));
		if (best > kGrid / 2)
			missing++;
		else
			worst = std::max(worst, best);
	}
	printf("%s: %zu lines found, %d missing, worst offset %.1f px\n", what, found.size(), missing, worst);
	CHECK(missing <= 1);  // the last periphery line may fall below threshold when averaged into 1:4 bands
	CHECK(worst <= 4.0); // the client stretches peripheral stream pixels up to 4x: nearest-sample error < 4 px
}

// upscaler: render scale 0.5 (Settings > VR "Render resolution"): the game renders a half-size grid (lines every
// 32 px, 2 px wide), the compositor upscales it to the headset's render size, and the headset must see the full-size
// grid at the same places as without the scale.
int64_t run(const char * label, bool wide, bool flip, const CompositorOptions * upscale = nullptr)
{
	// Quest 3 under WiVRn v26.9 (21:09 live log): render 1836x1924 per eye, stream 918x962 per eye
	const uint32_t eye_w = 1836, eye_h = 1924, out_w = 918, out_h = 962;
	const uint32_t div = upscale ? 2 : 1, rw = eye_w / div, rh = eye_h / div;
	IOSurfaceRef surf[2] = {};
	EyeSource src[2];
	if (wide)
	{
		surf[0] = grid_surface(2 * rw, rh, rw, kGrid / div, kLine / div);
		for (int e = 0; e < 2; e++)
			src[e] = {surf[0], uint32_t(MTLPixelFormatBGRA8Unorm), false, e * rw, 0, rw, rh, flip};
	}
	else
		for (int e = 0; e < 2; e++)
		{
			surf[e] = grid_surface(rw, rh, rw, kGrid / div, kLine / div);
			src[e] = {surf[e], uint32_t(MTLPixelFormatBGRA8Unorm), false, 0, 0, rw, rh, flip};
		}
	// Quest 3-like asymmetric per-eye FOV (radians: left, right, up, down)
	EyeFov fov[2] = {{-0.94f, 0.70f, 0.77f, -0.96f}, {-0.70f, 0.94f, 0.77f, -0.96f}};
	float eye_x[2] = {-0.032f, 0.032f};
	CompositorOptions opt = upscale ? *upscale : CompositorOptions{};
	if (upscale)
		opt.upscale_w = eye_w, opt.upscale_h = eye_h;
	EyeCompositor comp(out_w, out_h, opt);
	CVPixelBufferPoolRef pool = nv12_pool(out_w, out_h);
	CVPixelBufferRef out[2] = {};
	for (auto & b: out)
		CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &b);
	FoveationParams params[2];
	REQUIRE(comp.compose(src, fov, eye_x, out, params));
	if (upscale)
		CHECK(std::string(comp.last_upscaler()) == (opt.upscaler == CompositorOptions::Upscaler::metalfx ? "metalfx" : "bilinear"));
	else
		CHECK(std::string(comp.last_upscaler()).empty());
	// GPU cost: median of 20 more frames (the first one includes pipeline/scaler setup)
	std::vector<int64_t> gpu;
	for (int i = 0; i < 20; i++)
	{
		REQUIRE(comp.compose(src, fov, eye_x, out, params));
		gpu.push_back(comp.last_gpu_ns());
	}
	std::sort(gpu.begin(), gpu.end());
	printf("[%s] compose GPU p50 %.3f ms\n", label, gpu[10] / 1e6);
	for (int e = 0; e < 2; e++)
	{
		auto img = test::defoveate(out[e], params[e].x, params[e].y);
		printf("[%s] eye %d: params x %zu y %zu, defoveated %ux%u\n", label, e, params[e].x.size(), params[e].y.size(), img.w,
		       img.h);
		CHECK(img.w == eye_w and img.h == eye_h); // the client's image has the game's size, so the FOV maps 1:1
		char what[64];
		// rows / columns between grid lines: centre, and near the edges (strongest compression)
		for (uint32_t y: {eye_h / 2 + kGrid / 2, uint32_t(kGrid / 2 + 3 * kGrid), eye_h - 3 * kGrid})
		{
			snprintf(what, sizeof what, "[%s] eye %d row %u", label, e, y);
			check_axis(what, img, y - y % kGrid + kGrid / 2, true, eye_w, false);
		}
		for (uint32_t x: {eye_w / 2 + kGrid / 2, uint32_t(kGrid / 2 + 3 * kGrid), eye_w - 3 * kGrid})
		{
			snprintf(what, sizeof what, "[%s] eye %d column %u", label, e, x);
			// bottom-up source rows: the compositor turns them the right way up, so the headset's image is the grid
			// as the game meant it (row 0 at the top); the grid is not symmetric vertically (eye_h % 64 != 0)
			check_axis(what, img, x - x % kGrid + kGrid / 2, false, eye_h, flip);
		}
	}
	for (auto b: out)
		CVPixelBufferRelease(b);
	CVPixelBufferPoolRelease(pool);
	for (auto s: surf)
		if (s)
			CFRelease(s);
	return gpu[10];
}

// one uniform-colour 64x64 eye through the compositor; returns the centre luma
IOSurfaceRef flat_surface(uint8_t v, uint32_t n = 64, bool edge = false)
{
	NSDictionary * props = @{(id)kIOSurfaceWidth: @(n), (id)kIOSurfaceHeight: @(n), (id)kIOSurfaceBytesPerElement: @4,
	                         (id)kIOSurfacePixelFormat: @(uint32_t('BGRA'))};
	IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	IOSurfaceLock(s, 0, nullptr);
	auto * base = (uint8_t *)IOSurfaceGetBaseAddress(s);
	for (uint32_t y = 0; y < n; y++)
		for (uint32_t x = 0; x < n; x++)
		{
			uint8_t * p = base + y * IOSurfaceGetBytesPerRow(s) + x * 4;
			p[0] = p[1] = p[2] = edge ? (x < n / 2 ? 64 : 192) : v;
			p[3] = 255;
		}
	IOSurfaceUnlock(s, 0, nullptr);
	return s;
}

std::vector<uint8_t> luma_row(CompositorOptions opt, IOSurfaceRef s, bool srgb)
{
	const uint32_t n = 64;
	EyeCompositor comp(n, n, opt);
	CVPixelBufferPoolRef pool = nv12_pool(n, n);
	CVPixelBufferRef out[2] = {};
	for (auto & b: out)
		CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &b);
	EyeSource src[2] = {{s, uint32_t(MTLPixelFormatBGRA8Unorm), srgb, 0, 0, n, n, false},
	                    {s, uint32_t(MTLPixelFormatBGRA8Unorm), srgb, 0, 0, n, n, false}};
	EyeFov fov[2] = {{-0.8f, 0.8f, 0.8f, -0.8f}, {-0.8f, 0.8f, 0.8f, -0.8f}};
	float ex[2] = {-0.032f, 0.032f};
	FoveationParams params[2];
	std::vector<uint8_t> row;
	if (comp.compose(src, fov, ex, out, params))
	{
		CVPixelBufferLockBaseAddress(out[0], kCVPixelBufferLock_ReadOnly);
		auto * y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(out[0], 0) + n / 2 * CVPixelBufferGetBytesPerRowOfPlane(out[0], 0);
		row.assign(y, y + n);
		CVPixelBufferUnlockBaseAddress(out[0], kCVPixelBufferLock_ReadOnly);
	}
	for (auto b: out)
		CVPixelBufferRelease(b);
	CVPixelBufferPoolRelease(pool);
	return row;
}

// sharpening: a flat image stays flat, an edge gets more contrast; force_srgb: an sRGB-encoded mid grey that the
// game's format did not mark sRGB comes out as itself, not brightened by a second sRGB encode
void check_sharpness_and_srgb()
{
	IOSurfaceRef flat = flat_surface(128), edge = flat_surface(0, 64, true);
	CompositorOptions sharp;
	sharp.sharpness = 1;
	auto f0 = luma_row({}, flat, true), f1 = luma_row(sharp, flat, true);
	REQUIRE(f0.size() == 64 and f1.size() == 64);
	CHECK(std::abs(int(f0[32]) - int(f1[32])) <= 1);
	auto e0 = luma_row({}, edge, true), e1 = luma_row(sharp, edge, true);
	int c0 = int(e0[32]) - int(e0[31]), c1 = int(e1[32]) - int(e1[31]); // the two pixels at the edge
	printf("sharpness: flat %u -> %u, edge contrast %d -> %d\n", f0[32], f1[32], c0, c1);
	CHECK(c1 > c0);
	CompositorOptions force;
	force.force_srgb = true;
	auto lin = luma_row({}, flat, false), forced = luma_row(force, flat, false), marked = luma_row({}, flat, true);
	printf("srgb: unmarked %u, forced %u, marked %u\n", lin[32], forced[32], marked[32]);
	CHECK(lin[32] > marked[32] + 30);             // unmarked sRGB data is encoded twice: washed out
	CHECK(std::abs(int(forced[32]) - int(marked[32])) <= 1);
	CFRelease(flat);
	CFRelease(edge);
}

// The parameters (and the per-pixel source ranges) depend on the eye's x offset in head space. CompositorBridge
// derived it from stage-space x, which shrinks to 0 when the head turns 90 degrees: the foveation centre then moved
// with head yaw and the parameters were recomputed every frame.
void check_eye_x_stable()
{
	const float ipd = 0.064f;
	float prev[2] = {};
	for (int i = 0; i <= 8; i++)
	{
		float yaw = float(M_PI / 2) * i / 8;
		LinkFrame f{};
		for (int e = 0; e < 2; e++)
		{
			float off = (e ? 0.5f : -0.5f) * ipd;
			f.pose[e][0] = 0, f.pose[e][1] = std::sin(yaw / 2), f.pose[e][2] = 0, f.pose[e][3] = std::cos(yaw / 2);
			f.pose[e][4] = off * std::cos(yaw); // head x axis rotated by yaw about +y: (cos, 0, -sin)
			f.pose[e][5] = 1.6f;
			f.pose[e][6] = -off * std::sin(yaw);
		}
		float x[2];
		CompositorBridge::eye_offsets(f, x);
		if (i)
			CHECK(std::abs(x[0] - prev[0]) < 1e-6f and std::abs(x[1] - prev[1]) < 1e-6f);
		CHECK(std::abs(x[0] + ipd / 2) < 1e-4f and std::abs(x[1] - ipd / 2) < 1e-4f);
		prev[0] = x[0], prev[1] = x[1];
	}
	printf("eye x offsets independent of head yaw: %s\n", g_failures ? "no" : "yes");
}
} // namespace

int main()
{
	@autoreleasepool
	{
		check_eye_x_stable();
		int64_t base = run("separate", false, false);
		CompositorOptions fx, bl;
		bl.upscaler = CompositorOptions::Upscaler::bilinear;
		int64_t t_fx = run("scale 0.5 metalfx", false, false, &fx);
		int64_t t_bl = run("scale 0.5 bilinear", true, true, &bl);
		printf("upscale cost over the plain compose: metalfx +%.3f ms, bilinear +%.3f ms (both eyes)\n",
		       (t_fx - base) / 1e6, (t_bl - base) / 1e6);
		check_sharpness_and_srgb();
		run("double-wide", true, false);
		run("bottom-up", false, true);
	}
	return test_result("foveation_geometry");
}
