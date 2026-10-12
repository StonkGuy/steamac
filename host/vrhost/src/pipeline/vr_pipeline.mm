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

VrPipeline::VrPipeline(VrSession & session, int link_fd, const std::string & mach_name) : session(session)
{
	server = std::make_unique<VrLinkServer>(
	        link_fd, mach_name, [this](const LinkFrame & f) { on_frame(f); },
	        [this](const void * m, size_t n) { on_message(m, n); }, [this] { on_frame_error(); });
	clock = std::thread([this] { frame_clock(); });
}

VrPipeline::~VrPipeline()
{
	quit = true;
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

void VrPipeline::send_state(bool connected)
{
	vrl_state s{};
	s.h = {VRL_STATE, sizeof s};
	s.connected = connected;
	if (auto info = connected ? session.headset() : std::nullopt)
	{
		// Without host foveation the compositor only converts, so the guest renders at the stream size (A2).
		bool own_size = info->render_eye_width and info->foveation != FoveationModel::none;
		s.eye_width = own_size ? info->render_eye_width : info->eye_width;
		s.eye_height = own_size ? info->render_eye_height : info->eye_height;
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
// display time, and submits with that frame id (WiVRn matches the headset's feedback by it).
void VrPipeline::frame_clock()
{
	while (not quit and not server->peer_closed())
	{
		if (not session.connected())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		FrameTiming t = session.next_frame_timing();
		vrl_timing tm{};
		tm.h = {VRL_TIMING, sizeof tm};
		tm.frame_id = t.frame_id;
		tm.wake_up = t.wake_up;
		tm.predicted_display = t.predicted_display;
		tm.refresh_hz = refresh;
		vrl_poses ps{};
		ps.h = {VRL_POSES, sizeof ps};
		ps.time = t.predicted_display;
		for (int d = 0; d < VRL_DEV_COUNT; d++)
			ps.pose[d] = to_vrl(session.pose_at(Device(d), t.predicted_display));
		// poses first: the guest's frame starts on the timing packet
		server->send(&ps, sizeof ps);
		if (server->send(&tm, sizeof tm))
			timings++;
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
	auto info = session.connected() ? session.headset() : std::nullopt;
	if (not info or not info->eye_width)
	{
		st.no_headset++;
		return; // VrLinkServer releases the images
	}
	std::lock_guard lock(m);
	if (info->eye_width != w or info->eye_height != h)
	{
		bridge.reset();
		sink = std::make_unique<SessionEncoderSink>(session, info->eye_width, info->eye_height);
		bridge = std::make_unique<CompositorBridge>(info->eye_width, info->eye_height, *sink);
		w = info->eye_width;
		h = info->eye_height;
	}
	(*bridge)(f);
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
