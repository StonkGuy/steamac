// SPDX-License-Identifier: GPL-3.0-or-later
// vrhost-test-vd: a synthetic Virtual Desktop headset for a running steamac-vrhost --adapter vd (personal-use adapter,
// private repo). Speaks the VD LAN protocol like the Quest 3 app (fake_vd_client.cpp): discovery with the installed
// Streamer's key, the four channels, 72 Hz, Quest 3 FOV/IPD, real VideoToolbox decode, and real-headset conditions:
//   --seconds N            run time (default 60)
//   --motion calm|turns    head trace: slow look-around, or with fast 90-degree turns (~4 rad/s peak) (default turns)
//   --rate-mbps R          Wi-Fi throughput cap on the video channel (default 120)
//   --delay-ms D --jitter-ms J  tracking upstream delay/jitter (default 8 / 10)
//   --decode-fps F         headset decoder throughput (default 110)
//   --recenter-every N     send Recenter every N frames;  --ssw-toggle-every N  toggle UseSpacewarp every N frames
//   --ssw on|off           the headset's spacewarp setting at connect (default on); --quality N (default 0 = Low)
//   --csv FILE             one line per frame (arrival, index, poses, latency, decode)
// Checks (on decoded frames):
//   world lock / projection: between frames the picture must shift by what the frames' RenderPose + FovPort predict
//     (motion check in view-angle space, yaw and pitch) - a FOV/projection or pose mismatch shows as a ratio != 1;
//   pose naming: the RenderPose must equal the tracking sample the frame names (frame_index), as the headset sent it;
//   latency: sample sent -> frame arrived, p50/p99 per 30 s window (growth over time shows), drops/IDRs.
#import <Foundation/Foundation.h>
#include "fake_vd_client.h"
#include "session/clock.h"
#include "vd/vd_streamer_key.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace vrhost;
using namespace vrhost::vd;

namespace
{
// --- head motion -------------------------------------------------------------------------------------------------
struct Trace
{
	bool turns = true;
	// yaw/pitch (rad) at t seconds
	void at(double t, double & yaw, double & pitch) const
	{
		yaw = 0.6 * std::sin(2 * M_PI * 0.13 * t) + 0.15 * std::sin(2 * M_PI * 0.41 * t);
		pitch = 0.25 * std::sin(2 * M_PI * 0.09 * t + 1);
		if (turns)
		{
			// every 8 s a 90-degree turn in 0.5 s (smoothstep, peak ~4.7 rad/s), back 4 s later
			double c = std::fmod(t, 8.0);
			auto step = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); };
			yaw += M_PI / 2 * (step((c - 1) / 0.5) - step((c - 5) / 0.5));
		}
	}
};

Quat quat(double yaw, double pitch) // yaw about +Y, then pitch about the head's +X
{
	double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
	return Quat{float(cy * sp), float(sy * cp), float(-sy * sp), float(cy * cp)};
}

void angles(const Quat & q, float & yaw, float & pitch)
{
	float x = q.x, y = q.y, z = q.z, w = q.w;
	float fx = -(2 * (x * z + y * w)), fy = -(2 * (y * z - x * w)), fz = -(1 - 2 * (x * x + y * y));
	yaw = std::atan2(-fx, -fz);
	pitch = std::asin(std::clamp(fy, -1.f, 1.f));
}

// --- picture profiles + motion check (same method as vrhost-test-headset --scene) ------------------------------------
struct Sample
{
	float yaw = 0, pitch = 0, tan_l = 0, tan_r = 0, tan_u = 0, tan_d = 0;
	std::vector<float> cols, rows;
};

Sample make_sample(CVPixelBufferRef pb, const FrameMetadata & md)
{
	Sample s;
	angles(md.pose.eye[0].orientation, s.yaw, s.pitch);
	s.tan_l = md.pose.fov[0].left, s.tan_r = md.pose.fov[0].right, s.tan_u = md.pose.fov[0].up, s.tan_d = md.pose.fov[0].down;
	CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	auto * y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
	size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), w = CVPixelBufferGetWidthOfPlane(pb, 0),
	       h = CVPixelBufferGetHeightOfPlane(pb, 0);
	s.cols.assign(w, 0);
	s.rows.assign(h, 0);
	for (size_t r = h / 3; r < 2 * h / 3; r++)
		for (size_t c = 0; c < w; c++)
			s.cols[c] += y[r * stride + c];
	for (size_t r = 0; r < h; r++)
		for (size_t c = w / 3; c < 2 * w / 3; c++)
			s.rows[r] += y[r * stride + c];
	CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	return s;
}

int best_shift(const std::vector<float> & a, const std::vector<float> & b, int max_shift, double * score)
{
	int n = int(std::min(a.size(), b.size())), best = 0;
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

constexpr double kAngleStep = 0.25 * M_PI / 180;
std::vector<float> to_angles(const std::vector<float> & p, double tan_lo, double tan_hi)
{
	// tan_lo is the (positive) tangent on the low side: angles run from -atan(tan_lo) to +atan(tan_hi)
	double a0 = -std::atan(tan_lo), a1 = std::atan(tan_hi);
	std::vector<float> out;
	for (double a = a0; a < a1; a += kAngleStep)
	{
		double x = (std::tan(a) + tan_lo) / (tan_hi + tan_lo) * p.size() - 0.5;
		int i = std::clamp(int(std::floor(x)), 0, int(p.size()) - 2);
		double f = std::clamp(x - i, 0.0, 1.0);
		out.push_back(float(p[i] * (1 - f) + p[i + 1] * f));
	}
	return out;
}

struct MotionResult
{
	int pairs = 0, matched = 0;
	double median = 0, p10 = 0, p90 = 0;
	std::vector<double> ratio;
	void finish()
	{
		matched = int(ratio.size());
		if (ratio.empty())
			return;
		std::sort(ratio.begin(), ratio.end());
		median = ratio[ratio.size() / 2], p10 = ratio[ratio.size() / 10], p90 = ratio[ratio.size() * 9 / 10];
	}
};

// pairs starting in s[0 .. first_end) (later samples only as partners); appends to m
void motion_pairs(const std::vector<Sample> & s, size_t first_end, bool horizontal, MotionResult & m)
{
	auto & ratio = m.ratio;
	for (size_t i = 0; i + 1 < s.size() and i < first_end; ++i)
	{
		size_t j = i + 1;
		for (; j < s.size() and j < i + 6; ++j)
			if (std::abs(horizontal ? s[j].yaw - s[i].yaw : s[j].pitch - s[i].pitch) >= 2 * M_PI / 180)
				break;
		if (j >= s.size() or j >= i + 6)
			continue;
		const Sample & a = s[i], & b = s[j];
		float d = horizontal ? b.yaw - a.yaw : b.pitch - a.pitch;
		if (std::abs(d) > 20 * M_PI / 180)
			continue;
		auto prof = [&](const Sample & x) {
			if (horizontal)
				return to_angles(x.cols, x.tan_l, x.tan_r);
			std::vector<float> r(x.rows.rbegin(), x.rows.rend());
			return to_angles(r, x.tan_d, x.tan_u);
		};
		double expect = (horizontal ? d : -d) / kAngleStep;
		double score;
		int got = best_shift(prof(a), prof(b), int(std::abs(expect) * 3 + 20), &score);
		m.pairs++;
		if (getenv("VDTOOL_DEBUG") and m.pairs < 15)
			fprintf(stderr, "pair %zu-%zu d %.4f expect %.1f got %d score %.3f prof %zu\n", i, j, d, expect, got, score, prof(a).size());
		if (score < 0.6)
			continue;
		ratio.push_back(got / expect);
	}
}

double pct(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}
} // namespace

int main(int argc, char ** argv)
{
	@autoreleasepool
	{
		signal(SIGPIPE, SIG_IGN);
		std::string host = "127.0.0.1", csv_path;
		double seconds = 60, rate = 120, decode_fps = 110;
		int delay = 8, jitter = 10, recenter_every = 0, ssw_toggle_every = 0, quality = 0;
		bool ssw = true;
		Trace trace;
		for (int i = 1; i < argc; ++i)
		{
			std::string a = argv[i];
			auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
			if (a == "--host") host = next();
			else if (a == "--seconds") seconds = std::stod(next());
			else if (a == "--motion") trace.turns = next() != "calm";
			else if (a == "--rate-mbps") rate = std::stod(next());
			else if (a == "--delay-ms") delay = std::stoi(next());
			else if (a == "--jitter-ms") jitter = std::stoi(next());
			else if (a == "--decode-fps") decode_fps = std::stod(next());
			else if (a == "--recenter-every") recenter_every = std::stoi(next());
			else if (a == "--ssw-toggle-every") ssw_toggle_every = std::stoi(next());
			else if (a == "--ssw") ssw = next() != "off";
			else if (a == "--quality") quality = std::stoi(next());
			else if (a == "--csv") csv_path = next();
			else
			{
				fprintf(stderr, "unknown option %s (see the header of tests/test_vd_tool.mm)\n", a.c_str());
				return 2;
			}
		}
		auto key = read_installed_streamer_key();
		if (not key.key)
		{
			fprintf(stderr, "no discovery key: %s\n", key.error.c_str());
			return 2;
		}

		FakeVdOptions o;
		o.host = host;
		o.discovery_port = default_discovery_port, o.control_port = default_control_port, o.data_port = default_data_port;
		o.video_port = default_video_port, o.audio_port = default_audio_port;
		o.discovery_key = *key.key;
		o.account = "vrhost-test-vd";
		o.user_settings = "{\"VRFramerate\":72,\"VRGraphicsQuality\":" + std::to_string(quality) + "}";
		// the Quest 3 as the 2026-10-17 trial reported it (FovLeft/FovRight tangents, IPD)
		o.mobile_settings = std::string(R"({"HmdType":1000,"UseSpacewarp":)") + (ssw ? "true" : "false") +
		                    R"(,"IPD":0.064,"MeasuredBandwidth":)" + std::to_string(int64_t(rate * 1e6)) +
		                    R"(,"FovLeft":{"UpTan":0.97,"DownTan":1.43,"LeftTan":1.37,"RightTan":0.84},)"
		                    R"("FovRight":{"UpTan":0.97,"DownTan":1.43,"LeftTan":0.84,"RightTan":1.37},"Features":262144})";
		o.frames = int(seconds * 72) + 1000;
		o.timeout_ms = int(seconds * 1000);
		o.audio = true;
		o.keep_results = false;
		o.video_idle_ms = 600000; // the game may take minutes to submit its first frame
		o.video_rate_bps = rate * 1e6;
		o.net_delay_ms = delay, o.net_jitter_ms = jitter;
		o.decode_cap_fps = decode_fps;
		o.tracking_period_ms = 2; // ~500 Hz like the Quest app's upstream
		if (recenter_every > 0)
			for (int f = recenter_every; f < o.frames; f += recenter_every)
				o.control_schedule.push_back({f, uint8_t(Msg::recenter), std::string()});
		if (ssw_toggle_every > 0)
			for (int f = ssw_toggle_every, k = 0; f < o.frames; f += ssw_toggle_every, ++k)
				o.mobile_schedule.push_back({f, "UseSpacewarp", (k % 2 == 0) == ssw ? "false" : "true"});

		// tracking samples: what we sent, by index, with the send time
		struct Sent
		{
			int64_t t;
			Quat q;
		};
		std::mutex m;
		std::map<int32_t, Sent> sent;
		int64_t t0 = now_ns();
		o.tracking_fn = [&](int32_t n) {
			double t = (now_ns() - t0) / 1e9, yaw, pitch, y2, p2;
			trace.at(t, yaw, pitch);
			trace.at(t + 0.001, y2, p2);
			TrackingSample s;
			s.head.tracked = true;
			s.head.mounted = true;
			s.head.orientation = quat(yaw, pitch);
			s.head.position = {0, 1.2f, 0};
			// world-space angular velocity: yaw rate about +Y (pitch rate about the head's x, small)
			s.head.angular_velocity = {0, float((y2 - yaw) / 0.001), 0};
			s.latency_s = 0.06f;
			std::lock_guard l(m);
			sent[n] = {now_ns(), s.head.orientation};
			if (sent.size() > 20000)
				sent.erase(sent.begin());
			return s;
		};

		FILE * csv = csv_path.empty() ? nullptr : fopen(csv_path.c_str(), "w");
		if (csv)
			fprintf(csv, "t_s,frame_index,sample_age_ms,md_yaw,md_pitch,sample_yaw,named_err_deg,true_yaw_at_arrival,decoded,idr\n");
		std::vector<Sample> samples;
		MotionResult yaw_m, pitch_m;
		std::vector<double> named_err, win_age;
		std::vector<std::pair<double, std::vector<double>>> windows; // (start s, ages)
		int frames = 0, decoded = 0, idr = 0, unnamed = 0;
		o.on_frame = [&](const FrameMetadata & md, CVPixelBufferRef l, CVPixelBufferRef, int64_t arrival) {
			frames++;
			idr += md.is_sync_frame;
			double t = (arrival - t0) / 1e9;
			float myaw, mpitch;
			angles(md.pose.eye[0].orientation, myaw, mpitch);
			Sent sn{};
			bool have = false;
			{
				std::lock_guard g(m);
				if (auto it = sent.find(md.frame_index); it != sent.end())
					sn = it->second, have = true;
			}
			double age = have ? (arrival - sn.t) / 1e6 : -1, err = -1;
			float syaw = 0, spitch = 0;
			if (have)
			{
				angles(sn.q, syaw, spitch);
				err = std::hypot(myaw - syaw, mpitch - spitch) * 180 / M_PI;
				named_err.push_back(err);
				if (windows.empty() or t - windows.back().first >= 30)
					windows.push_back({t, {}});
				windows.back().second.push_back(age);
			}
			else
				unnamed++;
			double ty, tp;
			trace.at(t, ty, tp);
			if (l)
			{
				decoded++;
				samples.push_back(make_sample(l, md));
				if (samples.size() >= 400) // bounded memory on long runs: evaluate, keep the tail as partners
				{
					motion_pairs(samples, samples.size() - 6, true, yaw_m);
					motion_pairs(samples, samples.size() - 6, false, pitch_m);
					samples.erase(samples.begin(), samples.end() - 6);
				}
			}
			if (csv)
				fprintf(csv, "%.4f,%d,%.2f,%.4f,%.4f,%.4f,%.3f,%.4f,%d,%d\n", t, md.frame_index, age, myaw, mpitch, syaw, err, ty,
				        l != nullptr, md.is_sync_frame);
		};
		auto r = run_fake_vd_client(o);
		if (csv)
			fclose(csv);

		printf("vrhost-test-vd: %d frames, %d decoded, %d IDR, %d without a known tracking sample%s%s\n", frames, decoded,
		       idr, unnamed, r.error.empty() ? "" : ", ended: ", r.error.c_str());
		printf("pose naming: RenderPose vs the named sample: p50 %.3f deg, p99 %.3f deg, max %.3f deg\n", pct(named_err, .5),
		       pct(named_err, .99), pct(named_err, 1));
		for (auto & [ws, ages]: windows)
			printf("latency %4.0f-%4.0f s: sample -> frame arrived p50 %.1f ms p99 %.1f ms (%zu frames)\n", ws, ws + 30,
			       pct(ages, .5), pct(ages, .99), ages.size());
		motion_pairs(samples, samples.size(), true, yaw_m);
		motion_pairs(samples, samples.size(), false, pitch_m);
		yaw_m.finish();
		pitch_m.finish();
		auto & yaw = yaw_m;
		auto & pitch = pitch_m;
		printf("projection/world lock yaw: %d/%d pairs, picture shift / RenderPose+FOV prediction median %.3f p10 %.3f p90 %.3f\n",
		       yaw.matched, yaw.pairs, yaw.median, yaw.p10, yaw.p90);
		printf("projection/world lock pitch: %d/%d pairs, median %.3f p10 %.3f p90 %.3f\n", pitch.matched, pitch.pairs,
		       pitch.median, pitch.p10, pitch.p90);
		bool ok = decoded > 0 and pct(named_err, .99) < 0.05 and std::abs(yaw.median - 1) < 0.1 and
		          (pitch.matched == 0 or std::abs(pitch.median - 1) < 0.15);
		printf("%s\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
}
