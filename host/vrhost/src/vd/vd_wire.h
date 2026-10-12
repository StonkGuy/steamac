// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: wire encoding. Control-message integers are big-endian (BeWriter/BeReader); the struct
// blobs (video format, frame metadata, tracking/input) are little-endian raw copies of a naturally aligned layout,
// reproduced here as C++ structs with static_asserted sizes (Apple arm64 and the peer are both little-endian).
// Layouts follow the field lists of the interoperability notes; offsets the notes leave open are pinned by
// tests/test_vd_wire.cpp (see vd_assumptions.h).
#pragma once

#include "vd/vd_assumptions.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace vrhost::vd
{
struct Quat
{
	float x = 0, y = 0, z = 0, w = 1;
};
struct Vec3
{
	float x = 0, y = 0, z = 0;
};
struct FovPort // tangents of the half-angles, all positive
{
	float up = 0, down = 0, left = 0, right = 0;
};
struct EyePose
{
	Quat orientation;
	Vec3 position;
};
struct RenderPose
{
	uint8_t has_valid_pose = 0, left_rendered = 0, right_rendered = 0;
	EyePose eye[2];
	FovPort fov[2];
};
static_assert(sizeof(RenderPose) == 92);

struct HapticVibration
{
	float amplitude = 0;
	uint32_t packed = 0; // doubles as a cursor/fovea position in other modes; 0 here
};

struct FrameMetadata
{
	int64_t timestamp_us = 0;
	int64_t frame_index = 0;
	Source source = Source::vr;
	uint8_t is_sync_frame = 0, is_hdr = 0;
	float frame_time_ms = 0, game_time_ms = 0, encode_time_ms = 0;
	RenderPose pose;
	HapticVibration haptic[2];
	int32_t render_width = 0, render_height = 0;
	int32_t data_size = 0;
};
static_assert(sizeof(FrameMetadata) == 160);

struct SliceHeader
{
	int64_t timestamp_us = 0;
	int32_t stream_index = 0;
	int32_t data_size = 0;
};
static_assert(sizeof(SliceHeader) == 16);

struct Rect
{
	int32_t x = 0, y = 0, width = 0, height = 0;
};
struct StreamDesc
{
	int32_t width = 0, height = 0;
	Rect bounds;
};
struct VideoFormat
{
	Source source = Source::vr;
	VideoCodec codec = VideoCodec::hevc;
	float frame_rate = 0;
	int32_t width = 0, height = 0; // primary frame size (= per-eye encode size)
	int32_t stream_count = 0;
	StreamDesc stream[4];
};
static_assert(sizeof(VideoFormat) == 120);

// Upstream (data channel).
struct ControllerInput
{
	uint32_t buttons = 0, touches = 0;
	uint8_t trigger = 0, grip = 0; // 0..255
	int16_t thumb_x = 0, thumb_y = 0; // -32767..32767
};
static_assert(sizeof(ControllerInput) == 16);

struct RawInputState
{
	ControllerInput hand[2];
	uint8_t pointer[16]{};  // TODO(vd-live:framedata-layout): pointer/mouse/gamepad sub-state sizes
	uint8_t keyboard[24]{}; // RawKeyboardState is explicitly 24 bytes (notes)
	uint8_t mouse[16]{};
	uint8_t gamepad[16]{};
	int32_t gamepad_model = 0;
};
static_assert(sizeof(RawInputState) == 108);

struct TrackedDevice
{
	Quat orientation;
	Vec3 position, linear_velocity, angular_velocity;
	uint8_t orientation_tracked = 0, position_tracked = 0;
	float battery = 0;
};
static_assert(sizeof(TrackedDevice) == 60);

struct FrameData
{
	int64_t frame_index = 0;
	float latency_s = 0;
	float floor_height = 0;
	RawInputState input;
	uint32_t lock_states = 0;
	TrackedDevice head, controller[2];
};
static_assert(sizeof(FrameData) == 312);

template <typename T>
std::span<const uint8_t> bytes_of(const T & v)
{
	return {reinterpret_cast<const uint8_t *>(&v), sizeof v};
}

template <typename T>
T from_bytes(std::span<const uint8_t> b)
{
	if (b.size() < sizeof(T))
		throw std::runtime_error("vd: short struct");
	T v;
	memcpy(&v, b.data(), sizeof v);
	return v;
}

// Big-endian binary writer/reader of control payloads: 16/32/64-bit BE, strings = u16 BE length + UTF-8,
// IP address = family byte (4|16) + address bytes + u32 BE port.
class BeWriter
{
public:
	std::vector<uint8_t> out;
	void u8(uint8_t v) { out.push_back(v); }
	void u16(uint16_t v);
	void u32(uint32_t v);
	void u64(uint64_t v);
	void str(const std::string & s);
	void raw(std::span<const uint8_t> b) { out.insert(out.end(), b.begin(), b.end()); }
	void address(std::span<const uint8_t> addr, uint32_t port);
};

class BeReader
{
public:
	explicit BeReader(std::span<const uint8_t> b) : b(b) {}
	uint8_t u8();
	uint16_t u16();
	uint32_t u32();
	uint64_t u64();
	std::string str();
	std::span<const uint8_t> raw(size_t n);
	// family 4|16; returns the address bytes
	std::vector<uint8_t> address(uint32_t & port);
	size_t left() const { return b.size() - pos; }

private:
	std::span<const uint8_t> b;
	size_t pos = 0;
};

// 7-bit varint (low groups first, high bit = more), as the stream wrapper's chunk length.
void put_varint(std::vector<uint8_t> & out, uint32_t v);
// Parses at b; returns the value and sets `used`, or nullopt when b ends inside the varint / it is too long.
std::optional<uint32_t> get_varint(std::span<const uint8_t> b, size_t & used);

struct PeerInfo
{
	std::vector<uint8_t> address; // 4 or 16 bytes
	uint32_t port = 0;
	bool encrypt = false;
};
std::vector<uint8_t> encode_peer_info(const PeerInfo &);

// Control message: 1-byte type, u32 BE payload length, payload.
std::vector<uint8_t> control_message(Msg, std::span<const uint8_t> payload);
std::vector<uint8_t> setting_changed_payload(const std::string & name, const std::string & json_value);
} // namespace vrhost::vd
