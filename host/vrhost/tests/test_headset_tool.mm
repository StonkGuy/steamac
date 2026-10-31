// SPDX-License-Identifier: GPL-3.0-or-later
// vrhost-test-headset: the loopback test headset (fake_headset.cpp, WiVRn v26.9 client sequence + VideoToolbox decode)
// as a tool, for checking a running steamac-vrhost on this Mac without a real headset (docs/vr.md):
//   vrhost-test-headset [--host ADDR] [--port N] [--pin NNNNNN] [--pin-delay MS] [--frames N] [--size WxH] [--start-app ID] [--net-delay MS] [--net-jitter MS] [--net-loss PCT] [--net-mbps M] [--net-burst S:MS]
// Reports decoded frames per eye, frame numbers read from the image (d3d11bench --vr / test pattern blocks; readable
// when the stream size equals the eye size, i.e. no foveation), the eyes' colour difference (d3d11bench --vr: left
// red, right green), the foveation parameters received, and frame arrival intervals. Exit 0 when frames decoded,
// every readable frame number matched and the eyes differ as expected.
// --quest3: the Quest 3 under WiVRn v26.9 as the 2026-10-12 live log showed it (render 1836x1924 per eye, stream
// 918x962, 90 Hz, asymmetric FOV); --render WxH / --refresh HZ override parts of it. --yaw DEG / --pitch DEG /
// --motion-hz F move the head (sinusoids); --controllers sends grip/aim poses and inputs (trigger ramp, A presses).
// --dump DIR [--dump-every N]: PNGs of the defoveated eye images (what the client reprojects) with their view pose.
// --scene: the picture is a game's (VRChat), not d3d11bench: no frame-number / colour checks; instead the motion
// check: between decoded frames the picture must shift by what the view_info pose change predicts for its FOV
// (world-locked; a head-locked picture does not shift), horizontally for yaw, vertically for pitch.
#include "fake_headset.h"
#include "wivrn_defoveate.h"
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <cmath>
#include <map>
#include "pattern/test_pattern.h"
#include "session/clock.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

static double mean_cr(CVPixelBufferRef img)
{
	CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	auto * p = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 1);
	size_t stride = CVPixelBufferGetBytesPerRowOfPlane(img, 1), w = CVPixelBufferGetWidthOfPlane(img, 1),
	       h = CVPixelBufferGetHeightOfPlane(img, 1);
	double sum = 0;
	size_t n = 0;
	for (size_t y = h / 8; y < h; y += 4) // below the frame number blocks
		for (size_t x = 0; x < w; x += 4, ++n)
			sum += p[y * stride + 2 * x + 1];
	CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	return n ? sum / n : 0;
}

static double pct(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

// one decoded eye picture, reduced to what the motion check needs
struct Sample
{
	float yaw = 0, pitch = 0;         // of the view_info pose (the pose the game rendered with)
	float tan_l, tan_r, tan_u, tan_d; // its FOV
	uint32_t w = 0, h = 0;
	std::vector<float> cols, rows; // mean luma per column of the middle third of rows / per row of the middle third of columns
	std::vector<uint8_t> small;    // 1/8 luma thumbnail (eye comparison)
};

static Sample make_sample(const vrhost::test::Image & pic, const FakeHeadsetOptions::View & v, int eye)
{
	Sample s;
	auto & q = v.orientation[eye];
	// forward = R * (0, 0, -1)
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float fx = -(2 * (x * z + y * w)), fy = -(2 * (y * z - x * w)), fz = -(1 - 2 * (x * x + y * y));
	s.yaw = std::atan2(-fx, -fz);
	s.pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, fy)));
	s.tan_l = std::tan(v.fov[eye][0]), s.tan_r = std::tan(v.fov[eye][1]), s.tan_u = std::tan(v.fov[eye][2]),
	s.tan_d = std::tan(v.fov[eye][3]);
	s.w = pic.w, s.h = pic.h;
	s.cols.assign(pic.w, 0);
	s.rows.assign(pic.h, 0);
	for (uint32_t r = pic.h / 3; r < 2 * pic.h / 3; r++)
		for (uint32_t c = 0; c < pic.w; c++)
			s.cols[c] += pic.luma(c, r);
	for (uint32_t r = 0; r < pic.h; r++)
		for (uint32_t c = pic.w / 3; c < 2 * pic.w / 3; c++)
			s.rows[r] += pic.luma(c, r);
	for (uint32_t r = 0; r + 8 <= pic.h; r += 8)
		for (uint32_t c = 0; c + 8 <= pic.w; c += 8)
			s.small.push_back(pic.luma(c, r));
	return s;
}

// shift d (pixels) that best maps profile a onto b: b(x) ~ a(x - d); returns the normalised correlation in *score
static int best_shift(const std::vector<float> & a, const std::vector<float> & b, int max_shift, double * score)
{
	int n = int(a.size()), best = 0;
	double best_c = -2;
	for (int d = -max_shift; d <= max_shift; d++)
	{
		double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
		int k = 0;
		for (int x = std::max(0, d) + n / 10; x < std::min(n, n + d) - n / 10; x++)
		{
			double va = a[x - d], vb = b[x];
			sa += va, sb += vb, saa += va * va, sbb += vb * vb, sab += va * vb, k++;
		}
		if (k < n / 4)
			continue;
		double cov = sab / k - sa / k * sb / k, va = saa / k - sa / k * sa / k, vb = sbb / k - sb / k * sb / k;
		double c = va > 1e-6 and vb > 1e-6 ? cov / std::sqrt(va * vb) : -1;
		if (c > best_c)
			best_c = c, best = d;
	}
	*score = best_c;
	return best;
}

// a profile over pixel columns (rows) resampled to equal steps of view angle: in angle space a turn of the render pose
// by d moves every scene point by exactly d (horizontal: yaw; vertical: pitch, exact on the centre column band)
static constexpr double kAngleStep = 0.25 * M_PI / 180;
static std::vector<float> to_angles(const std::vector<float> & p, double tan_lo, double tan_hi)
{
	double a0 = std::atan(tan_lo), a1 = std::atan(tan_hi);
	std::vector<float> out;
	for (double a = a0; a < a1; a += kAngleStep)
	{
		double x = (std::tan(a) - tan_lo) / (tan_hi - tan_lo) * p.size() - 0.5;
		int i = std::clamp(int(std::floor(x)), 0, int(p.size()) - 2);
		double f = std::clamp(x - i, 0.0, 1.0);
		out.push_back(float(p[i] * (1 - f) + p[i + 1] * f));
	}
	return out;
}

// World lock: when the render pose turns by d the scene must move by d in view angle (left turn -> scene moves right,
// look up -> scene moves down); a head-locked picture does not move at all. Measured on angle-resampled profiles.
static bool motion_check(const char * axis, const std::map<uint64_t, Sample> & s, bool horizontal)
{
	std::vector<double> ratio;
	int pairs = 0, low_contrast = 0;
	for (auto i = s.begin(); i != s.end(); ++i)
	{
		auto j = std::next(i);
		for (; j != s.end(); ++j)
		{
			float d = horizontal ? j->second.yaw - i->second.yaw : j->second.pitch - i->second.pitch;
			if (std::abs(d) >= 3 * M_PI / 180)
				break;
		}
		if (j == s.end())
			continue;
		const Sample & a = i->second, & b = j->second;
		float d = horizontal ? b.yaw - a.yaw : b.pitch - a.pitch;
		// rows run top (tan up) to bottom (tan down): flip them so the profile runs bottom -> top like the angles
		auto prof = [&](const Sample & x) {
			if (horizontal)
				return to_angles(x.cols, x.tan_l, x.tan_r);
			std::vector<float> r(x.rows.rbegin(), x.rows.rend());
			return to_angles(r, x.tan_d, x.tan_u);
		};
		// horizontal: +yaw -> scene angle decreases... in image x it moves right = towards larger angle; vertical: +pitch
		// -> scene moves down = towards smaller angle
		double expect = (horizontal ? d : -d) / kAngleStep;
		double score;
		int got = best_shift(prof(a), prof(b), int(std::abs(expect) * 3 + 20), &score);
		pairs++;
		if (score < 0.6)
		{
			low_contrast++;
			continue;
		}
		ratio.push_back(got / expect);
	}
	if (ratio.empty())
	{
		printf("motion %s: %d frame pairs, %d without a clear match: NOT MEASURED (no %s motion or a flat picture)\n", axis, pairs,
		       low_contrast, axis);
		return pairs == 0; // only an axis with motion has to pass
	}
	std::sort(ratio.begin(), ratio.end());
	double med = ratio[ratio.size() / 2];
	int near1 = 0;
	for (double r: ratio)
		near1 += std::abs(r - 1) < 0.25;
	bool ok = std::abs(med - 1) < 0.2;
	printf("motion %s: %zu frame pairs matched (%d unclear), picture shift / predicted: median %.2f, p10 %.2f, p90 %.2f, "
	       "%d within 25%% -> %s\n",
	       axis, ratio.size(), low_contrast, med, ratio[ratio.size() / 10], ratio[ratio.size() * 9 / 10], near1,
	       ok ? "world-locked" : std::abs(med) < 0.2 ? "HEAD-LOCKED" : "WRONG SCALE");
	return ok;
}

// left and right pictures of the same frame must differ (two renders, not one image twice) but only by parallax
static void eye_check(const std::map<uint64_t, Sample> (&s)[2])
{
	int n = 0;
	double diff = 0, shift = 0;
	for (auto & [f, l]: s[0])
	{
		auto r = s[1].find(f);
		if (r == s[1].end() or r->second.small.size() != l.small.size())
			continue;
		double d = 0;
		for (size_t k = 0; k < l.small.size(); k++)
			d += std::abs(int(l.small[k]) - int(r->second.small[k]));
		diff += d / l.small.size();
		double score;
		shift += best_shift(l.cols, r->second.cols, 120, &score);
		n++;
	}
	if (n)
		printf("eyes: %d frames compared, mean |left - right| luma %.1f, mean right-vs-left column shift %.1f px\n", n, diff / n,
		       shift / n);
}

static void write_png(const vrhost::test::Image & pic, const std::string & path)
{
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CFDataRef data = CFDataCreate(nullptr, pic.rgb.data(), CFIndex(pic.rgb.size()));
	CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
	CGImageRef im = CGImageCreate(pic.w, pic.h, 8, 24, pic.w * 3, cs, kCGBitmapByteOrderDefault | kCGImageAlphaNone, dp, nullptr,
	                              false, kCGRenderingIntentDefault);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), CFIndex(path.size()), false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
	if (dst)
	{
		CGImageDestinationAddImage(dst, im, nullptr);
		CGImageDestinationFinalize(dst);
		CFRelease(dst);
	}
	CFRelease(url);
	CGImageRelease(im);
	CGDataProviderRelease(dp);
	CFRelease(data);
	CGColorSpaceRelease(cs);
}

int main(int argc, char ** argv)
{
	FakeHeadsetOptions o{.frames = 300, .eye_width = 1024, .eye_height = 1092, .timeout_ms = 60000, .identity_foveation = false};
	std::string dump_dir;
	int dump_every = 90;
	bool scene = false;
	auto parse_wh = [](const std::string & v, uint16_t & w, uint16_t & h) {
		unsigned a, b;
		if (sscanf(v.c_str(), "%ux%u", &a, &b) != 2)
			return false;
		w = uint16_t(a), h = uint16_t(b);
		return true;
	};
	for (int i = 1; i < argc; i++)
	{
		std::string f = argv[i];
		if (f == "--quest3")
		{
			o.render_width = 1836, o.render_height = 1924, o.eye_width = 918, o.eye_height = 962, o.refresh_hz = 90;
			o.fov = {{{-0.94f, 0.70f, 0.77f, -0.96f}, {-0.70f, 0.94f, 0.77f, -0.96f}}};
			o.identity_foveation = false;
			continue;
		}
		if (f == "--controllers")
		{
			o.controllers = true;
			continue;
		}
		if (f == "--scene")
		{
			scene = true;
			continue;
		}
		if (i + 1 >= argc)
			break;
		std::string a = argv[i], v = argv[++i];
		if (a == "--render")
		{
			parse_wh(v, o.render_width, o.render_height);
			o.identity_foveation = false;
			continue;
		}
		if (a == "--refresh")
		{
			o.refresh_hz = std::stof(v);
			continue;
		}
		if (a == "--yaw")
		{
			o.yaw_amplitude = std::stof(v) * float(M_PI) / 180;
			continue;
		}
		if (a == "--pitch")
		{
			o.pitch_amplitude = std::stof(v) * float(M_PI) / 180;
			continue;
		}
		if (a == "--motion-hz")
		{
			o.motion_hz = std::stof(v);
			continue;
		}
		if (a == "--dump")
		{
			dump_dir = v;
			continue;
		}
		if (a == "--dump-every")
		{
			dump_every = std::max(1, std::stoi(v));
			continue;
		}
		if (a == "--timeout")
		{
			o.timeout_ms = std::stoi(v) * 1000;
			continue;
		}
		if (a == "--port")
			o.port = std::stoi(v);
		else if (a == "--host")
			o.host = v;
		else if (a == "--pin-delay")
			o.pin_delay_ms = std::stoi(v);
		else if (a == "--pin")
			o.pin = v;
		else if (a == "--frames")
			o.frames = std::stoi(v);
		else if (a == "--start-app") // the WiVRn app list's "start": the launcher starts that game in the guest
			o.start_app = v;
		// Wi-Fi-like shaping of the video stream (fake_headset.h Network)
		else if (a == "--net-delay")
			o.net.delay_ms = std::stof(v);
		else if (a == "--net-jitter")
			o.net.jitter_ms = std::stof(v);
		else if (a == "--net-loss")
			o.net.loss_pct = std::stof(v);
		else if (a == "--net-mbps")
			o.net.mbps = std::stof(v);
		else if (a == "--net-burst") // EVERY_S:MS
			sscanf(v.c_str(), "%f:%f", &o.net.burst_every_s, &o.net.burst_ms);
		else if (a == "--size")
		{
			parse_wh(v, o.eye_width, o.eye_height);
		}
		else
		{
			fprintf(stderr, "usage: vrhost-test-headset [--host ADDR] [--port N] [--pin NNNNNN] [--pin-delay MS] [--frames N] [--size WxH]\n"
			                "  [--quest3] [--render WxH] [--refresh HZ] [--yaw DEG] [--pitch DEG] [--motion-hz F] [--controllers]\n"
			                "  [--dump DIR] [--dump-every N] [--scene] [--timeout S]\n");
			return 2;
		}
	}
	std::mutex m;
	int read_ok = 0, read_wrong = 0, unreadable = 0;
	double cr[2] = {}, n[2] = {};
	std::vector<double> interval_ms;
	int64_t last = 0;
	o.on_decoded = [&](int eye, uint64_t frame, CVPixelBufferRef img) {
		int64_t r = vrhost::TestPattern::read_frame_index(img);
		double c = mean_cr(img);
		std::lock_guard l(m);
		if (r < 0)
			unreadable++;
		else if (r == int64_t(frame & 0xffff))
			read_ok++;
		else if (read_wrong++ < 5)
			fprintf(stderr, "eye %d frame %llu carries frame %lld\n", eye, (unsigned long long)frame, (long long)r);
		cr[eye] += c;
		n[eye]++;
		if (eye == 0)
		{
			int64_t t = vrhost::now_ns();
			if (last)
				interval_ms.push_back((t - last) / 1e6);
			last = t;
		}
	};
	std::map<uint64_t, Sample> samples[2];
	o.on_view = [&](int eye, uint64_t frame, CVPixelBufferRef img, const FakeHeadsetOptions::View & v) {
		bool dump = not dump_dir.empty() and frame % uint64_t(dump_every) == 0;
		bool sample = scene and frame % 3 == 0;
		if (not dump and not sample)
			return;
		auto pic = vrhost::test::defoveate(img, v.foveation_x[eye], v.foveation_y[eye]);
		Sample s = make_sample(pic, v, eye);
		if (dump)
		{
			char name[512];
			snprintf(name, sizeof name, "%s/f%06llu-%s.png", dump_dir.c_str(), (unsigned long long)frame, eye ? "right" : "left");
			write_png(pic, name);
			printf("dump %s: %ux%u (stream %zux%zu), view yaw %.2f pitch %.2f deg, fov %.3f/%.3f/%.3f/%.3f\n", name, pic.w, pic.h,
			       CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img), s.yaw * 180 / M_PI, s.pitch * 180 / M_PI, v.fov[eye][0],
			       v.fov[eye][1], v.fov[eye][2], v.fov[eye][3]);
		}
		std::lock_guard l(m);
		samples[eye][frame] = std::move(s);
	};
	auto r = run_fake_headset(o);
	bool motion_ok = true;
	if (scene)
	{
		motion_ok = motion_check("yaw", samples[0], true) & motion_check("pitch", samples[0], false);
		eye_check(samples);
	}
	double cr0 = n[0] ? cr[0] / n[0] : 0, cr1 = n[1] ? cr[1] / n[1] : 0;
	printf("handshake %s, encrypted %d, decoded %d/%d, IDR %d/%d, frame numbers ok %d wrong %d unreadable %d, "
	       "mean Cr left %.1f right %.1f, foveation x %zu y %zu entries, frame interval ms p50 %.2f p99 %.2f, net dropped %d, error '%s'\n",
	       r.handshake_state.c_str(), r.encrypted, r.decoded[0], r.decoded[1], r.idr[0], r.idr[1], read_ok, read_wrong,
	       unreadable, cr0, cr1, r.foveation_x[0].size(), r.foveation_y[0].size(), pct(interval_ms, .5), pct(interval_ms, .99),
	       r.net_dropped, r.error.c_str());
	bool ok = r.error.empty() and r.decoded[0] >= o.frames and r.decoded[1] >= o.frames and
	          (scene ? motion_ok : read_wrong == 0 and cr0 > cr1 + 10);
	printf(ok ? "vrhost-test-headset: PASS\n" : "vrhost-test-headset: FAIL\n");
	return ok ? 0 : 1;
}
