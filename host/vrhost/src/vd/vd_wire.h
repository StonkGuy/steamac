// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: wire encoding. Control-message integers are big-endian (BeWriter/BeReader); the struct
// blobs on the video channel are little-endian raw copies of a naturally aligned layout, reproduced here as C++
// structs with static_asserted offsets (Apple arm64 and the peer are both little-endian). The tracking/input blob of
// the data channel is decoded field by field at fixed offsets (TrackingSample). Layouts: VD-FACT(framedata-layout),
// VD-FACT(eye-delivery) in vd_assumptions.h, pinned by tests/test_vd_wire.cpp.
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
	int32_t fovea_x = 0, fovea_y = 0; // top-left of the fovea rectangle with foveated streaming; 0 here
};
static_assert(sizeof(HapticVibration) == 12);

struct FrameMetadata
{
	int64_t timestamp_us = 0;
	int32_t frame_index = 0;
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

struct Size2
{
	int32_t width = 0, height = 0;
};
struct Rect
{
	int32_t x = 0, y = 0, width = 0, height = 0;
};
struct StreamDesc
{
	Size2 size;
	Rect bounds;
};
// The full (newer) layout, 112 bytes; the VR producer sends its first 88 bytes (3 stream slots) under tag 2.
struct VideoFormat
{
	Source source = Source::vr;
	VideoCodec codec = VideoCodec::hevc;
	int32_t frame_rate = 0;
	Size2 frame_size;    // per-eye stream size
	int32_t stream_count = 0;
	Rect bounds;         // (0, 0, per-eye render size)  TODO(vd-live:bounds): meaning inferred
	StreamDesc more[3];  // secondary (fovea size with foveated streaming), tertiary, quaternary
};
static_assert(sizeof(VideoFormat) == 112);

// --- data channel: tracking + input (decoded at fixed offsets) ---------------------------------------------------
struct ControllerState // both controllers in one block
{
	uint16_t buttons = 0, touches = 0;
	uint8_t trigger[2]{}, grip[2]{}; // [left, right], 0..255
	int16_t thumb_x[2]{}, thumb_y[2]{};
};
struct HeadPose
{
	Quat orientation;
	Vec3 position, velocity, angular_velocity;
	bool tracked = false, mounted = false;
	int8_t battery = 0;
};
enum ControllerTracked : uint8_t
{
	controller_tracked = 1,
	controller_hand_tracked = 2,
};
struct ControllerPose
{
	Quat orientation;
	Vec3 position, velocity, acceleration, angular_velocity;
	uint8_t tracked = 0; // ControllerTracked flags
	int8_t battery = 0;
};
struct TrackingSample
{
	int32_t frame_index = 0;
	float latency_s = 0, floor_height = 0;
	ControllerState input;
	uint8_t lock_states = 0;
	HeadPose head;
	ControllerPose controller[2];
	size_t size = frame_data_base; // decoded length: which feature blocks (hands, body, eye, face) were present
};
// Field offsets inside the decompressed struct (VD-FACT(framedata-layout)).
namespace fd_off
{
inline constexpr size_t frame_index = 0, latency = 4, floor_height = 8, controller = 94, gamepad_model = 112,
                        lock_states = 116, head = 120, left_controller = 176, right_controller = 244;
}
std::optional<TrackingSample> decode_tracking(std::span<const uint8_t> raw); // nullopt when shorter than the base
std::vector<uint8_t> encode_tracking(const TrackingSample &, size_t size = frame_data_base); // test peer

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

// Control message: 1-byte type, u32 BE payload length, payload.
std::vector<uint8_t> control_message(Msg, std::span<const uint8_t> payload);
std::vector<uint8_t> control_message(Msg, const std::string & payload);
// A settings change: {"name":json_value}.
// VD-FACT(games-list): Games payload from (platform, id, running); parse of a LaunchGame/ExitGame payload.
struct GameEntry
{
	GamePlatform platform = GamePlatform::steam;
	uint64_t id = 0;
	bool running = false;
};
std::vector<uint8_t> games_payload(std::span<const GameEntry>);
std::optional<std::vector<GameEntry>> parse_games(std::span<const uint8_t>); // test peer
std::optional<GameEntry> parse_game_id(std::span<const uint8_t>);
std::vector<uint8_t> game_id_payload(GamePlatform, uint64_t); // test peer
std::string setting_changed_payload(const std::string & name, const std::string & json_value);
// Video channel messages.
std::vector<uint8_t> video_format_message(const VideoFormat &); // tag 2 + 88 bytes
std::vector<uint8_t> frame_message(const FrameMetadata &, std::span<const uint8_t> data);
std::vector<uint8_t> slice_message(const SliceHeader &, std::span<const uint8_t> data);
} // namespace vrhost::vd
