// SPDX-License-Identifier: GPL-3.0-or-later
// Wire-protocol-neutral VR session interface of steamac-vrhost. Contract: docs/vr-interfaces.md section 2.
// Nothing here includes WiVRn headers; adapters (WiVRn now, ALVR later) implement VrSession.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <IOSurface/IOSurfaceRef.h>

namespace vrhost
{
using ns = int64_t; // host monotonic clock, see clock.h

struct Pose
{
	std::array<float, 4> orientation{0, 0, 0, 1}; // x, y, z, w
	std::array<float, 3> position{0, 0, 0};
};

struct Fov // radians, OpenXR convention: left and down are negative
{
	float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;
};

enum class Device : uint8_t
{
	head,
	left_grip,
	left_aim,
	right_grip,
	right_aim,
	count,
};

enum class Codec : uint8_t
{
	h264,
	hevc,
	av1,
	raw,
};

// Interleaved S16 PCM as the headset plays/records it.
struct AudioFormat
{
	uint32_t channels = 0, rate = 0;
};

struct HeadsetInfo
{
	uint32_t eye_width = 0, eye_height = 0; // stream size per eye
	uint32_t render_eye_width = 0, render_eye_height = 0;
	// the headset's own render size, before the host's render scale (render_eye_* is what the game is asked for)
	uint32_t nominal_render_width = 0, nominal_render_height = 0;
	std::vector<float> refresh_rates;
	float preferred_refresh_rate = 0;
	std::array<Fov, 2> fov;
	std::vector<Codec> codecs; // most preferred first
	bool hand_tracking = false, eye_gaze = false;
	std::string name;
	std::optional<AudioFormat> speaker, microphone; // absent: the headset has none
};

struct TrackedPose
{
	Device device;
	ns time;
	Pose pose;
	std::array<float, 3> linear_velocity{}, angular_velocity{};
	bool orientation_valid = false, position_valid = false;
};

struct InputValue
{
	uint32_t id; // adapter-neutral input id (M2/M3 defines the table; WiVRn device_id values for now)
	float value;
	ns last_change;
};

struct FrameTiming
{
	ns wake_up, predicted_display;
	// Frame id the session assigned to this frame (WiVRn's pacer counts frames itself and matches the headset's
	// feedback by it): the producer must submit the frame with Frame::frame_id = this value.
	uint64_t frame_id = 0;
	// The headset's display period and how many display periods one rendered frame spans: render_divisor 2 = the
	// producer runs at half the display rate and the headset synthesises the frames in between (spacewarp);
	// predicted_display is still the display time this frame is shown first. 0 = not reported.
	ns display_period = 0;
	uint32_t render_divisor = 1;
};

// One eye of a submitted frame. The image is the ENCODER input: an IOSurface in
// kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ('420f', BT.709 full range, what WiVRn clients expect) of exactly
// the negotiated stream size. Converting the application's RGBA eye texture to this is the compositor's job (M1).
struct EyeImage
{
	IOSurfaceRef surface = nullptr;
};

// WiVRn foveation_parameter per eye (source-pixel counts per ratio, middle entry 1:1, see compositor/foveation_params.h).
// Empty = identity (no foveation).
struct Foveation
{
	std::vector<uint16_t> x, y;
};

struct Frame
{
	uint64_t frame_id = 0;
	ns display_time = 0;          // predicted display time (host clock) the frame was rendered for
	std::array<Pose, 2> pose;     // per-eye pose the app rendered with, in tracking (stage) space
	std::array<Fov, 2> fov;
	std::array<EyeImage, 2> eye;
	std::array<Foveation, 2> foveation; // what the compositor applied; sent with the stream so the client undoes it
};

class SessionListener
{
public:
	virtual ~SessionListener() = default;
	virtual void on_connected(const HeadsetInfo &) {}
	virtual void on_disconnected() {}
	virtual void on_tracking(std::span<const TrackedPose>) {}
	virtual void on_input(std::span<const InputValue>) {}
	virtual void on_refresh_rate(float hz) {}
	// Headset microphone PCM, HeadsetInfo::microphone format (only while set_microphone(true)).
	virtual void on_microphone(std::span<const int16_t> pcm, ns time) {}
};

class VrSession
{
public:
	virtual ~VrSession() = default;
	virtual void start(SessionListener *) = 0; // advertise + accept a headset (background threads)
	virtual void stop() = 0;
	virtual bool connected() const = 0;
	virtual std::optional<HeadsetInfo> headset() const = 0;
	// Blocks until it is time to start the next frame; returns the display time it should be rendered for.
	virtual FrameTiming next_frame_timing() = 0;
	// Per-eye poses + FOVs at a host time (interpolated/extrapolated from tracking history).
	virtual bool views_at(ns t, std::array<Pose, 2> & pose, std::array<Fov, 2> & fov) = 0;
	virtual std::optional<TrackedPose> pose_at(Device, ns t) = 0;
	// Encodes and sends; the IOSurfaces may be reused when it returns.
	virtual void submit(const Frame &) = 0;
	virtual void haptics(Device, float duration_s, float frequency, float amplitude) = 0;
	virtual void request_idr() = 0;
	// Speaker PCM for the headset, HeadsetInfo::speaker format; send at the audio quantum (5 ms), not buffered.
	virtual void send_audio(std::span<const int16_t> pcm) = 0;
	// Ask the headset to start/stop sending its microphone (WiVRn feature_control). Remembered across reconnects.
	virtual void set_microphone(bool on) = 0;
};

} // namespace vrhost
