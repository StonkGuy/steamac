// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>
#include "vr_pipeline.h"
#include "session/clock.h"
#include "vrlink.h"
#include "wivrn/input_map.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace vrhost
{
SessionEncoderSink::SessionEncoderSink(VrSession & session, uint32_t w, uint32_t h) : session(session)
{
	NSDictionary * attrs = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
		(id)kCVPixelBufferWidthKey: @(w),
		(id)kCVPixelBufferHeightKey: @(h),
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
	};
	if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attrs, &pool) != kCVReturnSuccess)
		throw std::runtime_error("CVPixelBufferPoolCreate failed");
}

SessionEncoderSink::~SessionEncoderSink()
{
	for (auto b: pending)
		if (b)
			CVPixelBufferRelease(b);
	CVPixelBufferPoolRelease(pool);
}

CVPixelBufferRef SessionEncoderSink::acquire(int)
{
	CVPixelBufferRef b = nullptr;
	CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &b);
	return b;
}

void SessionEncoderSink::encode(int eye, CVPixelBufferRef buffer, const LinkFrame & lf, const FoveationParams & p)
{
	if (pending[eye])
		CVPixelBufferRelease(pending[eye]);
	pending[eye] = CVPixelBufferRetain(buffer);
	params[eye] = p;
	if (eye != 1 or not pending[0])
		return;
	Frame f;
	f.frame_id = lf.frame_id;
	f.display_time = lf.display_time;
	for (int e = 0; e < 2; ++e)
	{
		f.pose[e].orientation = {lf.pose[e][0], lf.pose[e][1], lf.pose[e][2], lf.pose[e][3]};
		f.pose[e].position = {lf.pose[e][4], lf.pose[e][5], lf.pose[e][6]};
		f.fov[e] = {lf.fov[e].left, lf.fov[e].right, lf.fov[e].up, lf.fov[e].down};
		f.eye[e].surface = CVPixelBufferGetIOSurface(pending[e]);
		f.foveation[e] = {params[e].x, params[e].y};
	}
	session.submit(f); // synchronous: encoded and sent
	for (auto & b: pending)
	{
		CVPixelBufferRelease(b);
		b = nullptr;
	}
}

VrPipeline::VrPipeline(VrSession & session, int link_fd, const std::string & mach_name, CompositorOptions opt,
                       uint32_t frames_in_flight) :
        session(session), opt(opt), in_flight(frames_in_flight)
{
	server = std::make_unique<VrLinkServer>(
	        link_fd, mach_name, [this](const LinkFrame & f) { on_frame(f); },
	        [this](const void * m, size_t n) { on_message(m, n); }, [this] { on_frame_error(); });
	clock = std::thread([this] { frame_clock(); });
}

const char * VrPipeline::upscaler() const
{
	return last_upscaler.load();
}

VrPipeline::~VrPipeline()
{
	quit = true;
	in_flight.shutdown();
	if (clock.joinable())
		clock.join();
	server.reset(); // joins the link thread before the bridge goes away
}

static vrl_pose to_vrl(const std::optional<TrackedPose> & t)
{
	vrl_pose p{};
	p.orientation[3] = 1;
	if (not t)
		return p;
	std::copy(t->pose.orientation.begin(), t->pose.orientation.end(), p.orientation);
	std::copy(t->pose.position.begin(), t->pose.position.end(), p.position);
	std::copy(t->linear_velocity.begin(), t->linear_velocity.end(), p.lin_vel);
	std::copy(t->angular_velocity.begin(), t->angular_velocity.end(), p.ang_vel);
	p.flags = (t->orientation_valid ? VRL_POSE_VALID : 0) | (t->position_valid ? VRL_POSE_TRACKED : 0) | VRL_POSE_VEL;
	return p;
}

vrl_state VrPipeline::default_view()
{
	// Meta Quest 3 under WiVRn v26.9 (live 2026-10-16/17/19): 1836x1924 per eye, 90 Hz, its asymmetric FOV as the
	// headset reports it (left eye -54/+40 deg horizontal, +44/-55 deg vertical; right eye mirrored). A game that
	// starts before the headset keeps this projection, so it has to be the real headset's.
	vrl_state s{};
	s.h = {VRL_STATE, sizeof s};
	s.eye_width = 1836;
	s.eye_height = 1924;
	s.refresh_hz = 90;
	s.ipd_m = 0.064f;
	const float fov[2][4] = {{-0.94f, 0.70f, 0.77f, -0.96f}, {-0.70f, 0.94f, 0.77f, -0.96f}};
	memcpy(s.fov, fov, sizeof fov);
	s.profile = VRL_PROFILE_TOUCH;
	snprintf(s.name, sizeof s.name, "Meta Quest 3");
	return s;
}

void VrPipeline::send_state(bool connected)
{
	vrl_state s{};
	s.h = {VRL_STATE, sizeof s};
	s.connected = connected;
	if (auto info = connected ? session.headset() : std::nullopt)
	{
		s.eye_width = info->render_eye_width ? info->render_eye_width : info->eye_width;
		s.eye_height = info->render_eye_height ? info->render_eye_height : info->eye_height;
		s.refresh_hz = refresh ? refresh.load() : info->preferred_refresh_rate;
		for (int e = 0; e < 2; e++)
		{
			s.fov[e][0] = info->fov[e].left;
			s.fov[e][1] = info->fov[e].right;
			s.fov[e][2] = info->fov[e].up;
			s.fov[e][3] = info->fov[e].down;
		}
		std::array<Pose, 2> eye;
		std::array<Fov, 2> fov;
		session.views_at(now_ns(), eye, fov);
		float d2 = 0;
		for (int i = 0; i < 3; i++)
			d2 += (eye[1].position[i] - eye[0].position[i]) * (eye[1].position[i] - eye[0].position[i]);
		s.ipd_m = d2 > 0 ? std::sqrt(d2) : 0.064f;
		s.profile = VRL_PROFILE_TOUCH; // WiVRn clients: Quest/Pico touch-style controllers
		snprintf(s.name, sizeof s.name, "%s", info->name.c_str());
		last_view = s;
	}
	else
	{
		// No headset (not yet, or it left): the game keeps an HMD with the last headset's view, or a Quest 3's before
		// the first one, as SteamVR keeps an HMD in standby. A game that starts now never sees a 0x0 / zero-FOV
		// headset, and one already running keeps its projection; only `connected` changes.
		s = last_view.eye_width ? last_view : default_view();
		s.connected = 0;
	}
	server->send(&s, sizeof s);
}

void VrPipeline::on_connected(const HeadsetInfo & h)
{
	refresh = h.preferred_refresh_rate;
	send_state(true);
}

void VrPipeline::on_disconnected()
{
	{
		std::lock_guard l(input_mutex);
		memset(input, 0, sizeof input);
		input_time = now_ns();
	}
	send_state(false);
	send_input();
}

void VrPipeline::on_refresh_rate(float hz)
{
	refresh = hz;
	send_state(true);
}

void VrPipeline::send_input()
{
	vrl_input m{};
	m.h = {VRL_INPUT, sizeof m};
	m.profile = VRL_PROFILE_TOUCH;
	{
		std::lock_guard l(input_mutex);
		m.time = input_time;
		memcpy(m.value, input, sizeof m.value);
	}
	server->send(&m, sizeof m);
}

void VrPipeline::on_input(std::span<const InputValue> values)
{
	vrl_input m{};
	{
		std::lock_guard l(input_mutex);
		bool changed = false;
		for (auto & v: values)
		{
			int hand, index;
			if (not wivrn_input_to_vrl(v.id, hand, index))
				continue;
			changed |= input[hand][index] != v.value;
			input[hand][index] = v.value;
			input_time = std::max<int64_t>(input_time, v.last_change);
		}
		if (not changed)
			return;
		m.h = {VRL_INPUT, sizeof m};
		m.time = input_time;
		m.profile = VRL_PROFILE_TOUCH;
		memcpy(m.value, input, sizeof m.value);
	}
	server->send(&m, sizeof m);
}

void VrPipeline::on_message(const void * msg, size_t n)
{
	auto h = static_cast<const vrl_header *>(msg);
	if (h->type == VRL_HELLO and n == sizeof(vrl_hello))
	{
		send_state(session.connected());
		send_input();
		return;
	}
	if (h->type == VRL_HAPTICS and n == sizeof(vrl_haptics))
	{
		auto hp = static_cast<const vrl_haptics *>(msg);
		session.haptics(hp->hand ? Device::right_grip : Device::left_grip, hp->duration_s, hp->frequency, hp->amplitude);
		haptics++;
	}
}

// The pacer's frame cadence, forwarded: the guest's WaitGetPoses returns on VRL_TIMING with the poses for its
// display time, and submits with that frame id (WiVRn matches the headset's feedback by it). The render divisor goes
// along, so that the guest waits for the pacer's next frame instead of rendering frames in between.
void VrPipeline::frame_clock()
{
	uint64_t issued = 0; // the last frame id sent to the guest
	while (not quit and not server->peer_closed())
	{
		if (not session.connected())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		if (in_flight.enabled())
			in_flight.wait(issued, ns(1e9 / std::max(refresh.load(), 1.0f)), now_ns());
		FrameTiming t = session.next_frame_timing();
		issued = t.frame_id;
		vrl_timing tm{};
		tm.h = {VRL_TIMING, sizeof tm};
		tm.frame_id = t.frame_id;
		tm.wake_up = t.wake_up;
		tm.predicted_display = t.predicted_display;
		tm.refresh_hz = refresh;
		tm.render_divisor = t.render_divisor;
		vrl_poses ps{};
		ps.h = {VRL_POSES, sizeof ps};
		ps.time = t.predicted_display;
		for (int d = 0; d < VRL_DEV_COUNT; d++)
			ps.pose[d] = to_vrl(session.pose_at(Device(d), t.predicted_display));
		// poses first: the guest's frame starts on the timing packet
		server->send(&ps, sizeof ps);
		if (server->send(&tm, sizeof tm))
		{
			timings++;
			in_flight.released(t.frame_id, now_ns());
		}
	}
}

bool VrPipeline::peer_closed() const
{
	return server->peer_closed();
}

void VrPipeline::on_frame_error()
{
	std::lock_guard lock(m);
	bridge.reset();
	sink.reset();
	w = h = 0;
}

void VrPipeline::on_frame(const LinkFrame & f)
{
	st.received++;
	if (in_flight.enabled())
		in_flight.ready(f.frame_id, f.received_time, f.app_submit_time, f.ready_time);
	auto info = session.connected() ? session.headset() : std::nullopt;
	if (not info or not info->eye_width)
	{
		st.no_headset++;
		return; // VrLinkServer releases the images
	}
	std::lock_guard lock(m);
	CompositorOptions o = opt;
	if (info->nominal_render_width > info->render_eye_width) // render scale < 1: upscale to the headset's size
	{
		o.upscale_w = info->nominal_render_width;
		o.upscale_h = info->nominal_render_height;
	}
	if (info->eye_width != w or info->eye_height != h or not bridge or o.upscale_w != up_w)
	{
		bridge.reset();
		sink = std::make_unique<SessionEncoderSink>(session, info->eye_width, info->eye_height);
		bridge = std::make_unique<CompositorBridge>(info->eye_width, info->eye_height, *sink, o);
		up_w = o.upscale_w;
		w = info->eye_width;
		h = info->eye_height;
	}
	// what the game submits (after the exporter cut out its rect / slice): logged when it changes, so a live run shows
	// whether each eye gets an eye-sized image or a whole double-wide frame
	std::array<uint32_t, 6> geo{f.eye[0].w, f.eye[0].h, f.eye[1].w, f.eye[1].h, f.eye[0].pixel_format,
	                            uint32_t(f.eye[0].flip_y) | uint32_t(f.eye[0].srgb) << 1};
	if (geo != last_geometry)
	{
		last_geometry = geo;
		printf("[vrhost] game eye images: left %ux%u right %ux%u, format %u%s%s; headset eye %ux%u, render fov L %.2f/%.2f/%.2f/%.2f\n",
		       geo[0], geo[1], geo[2], geo[3], geo[4], f.eye[0].flip_y ? ", bottom-up" : "", f.eye[0].srgb ? ", sRGB" : "",
		       info->render_eye_width, info->render_eye_height, f.fov[0].left, f.fov[0].right, f.fov[0].up, f.fov[0].down);
		fflush(stdout);
	}
	// A frame rendered with a zero FOV (a runtime that got no view, an older guest): foveate with the headset's own
	// FOV instead of degenerate parameters.
	const LinkFrame * frame = &f;
	LinkFrame fixed;
	if (f.fov[0].right - f.fov[0].left <= 0 or f.fov[1].right - f.fov[1].left <= 0)
	{
		fixed = f;
		for (int e = 0; e < 2; e++)
			fixed.fov[e] = {info->fov[e].left, info->fov[e].right, info->fov[e].up, info->fov[e].down};
		frame = &fixed;
	}
	(*bridge)(*frame);
	last_upscaler = bridge->last_upscaler();
	int64_t sent = now_ns();
	st.submitted++;
	st.last_compose_gpu_ns = bridge->last_timing().compose_gpu_ns;
	st.last_latency_ns = sent - f.app_submit_time;
	std::lock_guard l(lat_mutex);
	if (lat.total.size() < 100'000)
	{
		lat.submit_to_copied.push_back((f.ready_time - f.app_submit_time) / 1e6);
		lat.copied_to_received.push_back((f.received_time - f.ready_time) / 1e6);
		lat.received_to_sent.push_back((sent - f.received_time) / 1e6);
		lat.total.push_back((sent - f.app_submit_time) / 1e6);
		lat.compose_gpu.push_back(bridge->last_timing().compose_gpu_ns / 1e6);
	}
}

PipelineLatency VrPipeline::take_latency()
{
	std::lock_guard l(lat_mutex);
	return std::exchange(lat, {});
}
} // namespace vrhost
