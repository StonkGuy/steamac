// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: every protocol fact the adapter depends on is decided HERE. Each one carries a tag naming
// the entry of the private evidence ledger (local only, never in this tree) that backs it:
//   VD-FACT(<name>)      read from the user's own installed Streamer / the official Streamer (static interoperability
//                        analysis), or measured on its data structures;
//   TODO(vd-live:<name>) still an inference, to be confirmed or corrected by the first trial with the user's headset.
// tests/test_vd_wire.cpp pins each value, so changing one is a deliberate, visible edit.
// Own code; no vendor code or keys (the discovery key is read at run time from the installed Streamer, see
// vd_streamer_key.h). Numbers below are wire values the headset expects, written in our own terms.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vrhost::vd
{
// --- Ports: VD-FACT(ports). LAN: the Streamer listens on these fixed ports; the headset dials them (the computer
// record carries no ports). --------------------------------------------------------------------------------------
inline constexpr uint16_t default_control_port = 38810;
inline constexpr uint16_t default_data_port = 38820;
inline constexpr uint16_t default_video_port = 38830;
inline constexpr uint16_t default_audio_port = 38840;
inline constexpr uint16_t default_discovery_port = 38850;

// VD-FACT(version): the record carries a System.Version; the headset (not the Streamer) judges it and, if it wants a
// newer Streamer, sends Msg::update_required, answered with Msg::update_unavailable.
inline constexpr int streamer_version[4] = {1, 34, 25, 0};

// --- Discovery: VD-FACT(discovery-cipher, connect-now, offer-layout, rsa-padding, reply-layout, record-schema) -------
// connect now: 17 bytes, NOT encrypted: the record's 16-byte connection id + 1 byte platform.
inline constexpr std::size_t connect_now_size = 17;
// offer: wrapped in the stream-chunk format (below) under the shared discovery key; inside: exactly 243 ASCII chars
// of a 1024-bit RSA public key in .NET XML form, then optionally {0x00, platform}, then the UTF-8 account id.
inline constexpr std::size_t offer_key_chars = 243;
// reply: RSA-OAEP(SHA-1) block (128 bytes) of a fresh AES-256 key + IV (48 bytes), then the computer record (.NET
// DataContract XML) in the stream-chunk format under that fresh key. The record itself carries the CHANNEL key.
inline constexpr std::size_t rsa_block_size = 128;
inline constexpr const char * record_namespace = "http://schemas.datacontract.org/2004/07/VirtualDesktop.Interfaces";
inline constexpr const char * os_windows = "Windows"; // user decision: the adapter reports Windows
enum class Region : uint8_t
{
	america_west,
	america_central,
	europe,
};

// --- Stream wrapper: VD-FACT(stream-cipher, plain-framing) --------------------------------------------------------
// AES-256-CBC, one chain per channel and direction from the channel IV; each write = one chunk = 7-bit varint length
// + payload + random padding to 16 bytes. Without encryption the same self-delimiting messages go raw.
// On the LAN the control channel is always encrypted; data/video/audio only when the record says EncryptLocalTraffic.
// VD-FACT(peer-info): the LAN path has no PeerInfo exchange (it belongs to the relay path).
// VD-FACT(channel-order): the Streamer accepts control, video, audio, data in that order; after the four are up it
// sends the bandwidth probe on the data channel: BE i32 n + n random bytes (n = 0 without auto bitrate).

// --- Control messages: VD-FACT(control-ids). 1-byte type, BE i32 payload length, payload. -------------------------
enum class Msg : uint8_t
{
	peer_info = 1,
	update_required = 4,
	update_unavailable = 5,
	update_in_progress = 6,
	reload_user_settings = 11,
	user_setting_changed = 12,
	reload_mobile_settings = 13,
	mobile_setting_changed = 14,
	reload_streamer_settings = 15,
	streamer_setting_changed = 16,
	boundary_data_changed = 21,
	skeleton_data_changed = 22,
	restart_stream = 23,
	force_key_frame = 24,
	switch_monitor = 25,
	launch_steamvr = 27,
	exit_steamvr = 28,
	recenter = 29,
	request_games = 30,
	games = 31,
	request_game_thumbnails = 32,
	game_thumbnail = 33,
	launch_game = 34,
	exit_game = 35,
	game_exited = 36,
	exit = 71,
	enable_vr_passthrough = 72,
	disable_vr_passthrough = 73,
	toggle_vr_passthrough = 74,
	toggle_hand_passthrough = 75,
	toggle_desk_passthrough = 76,
	toggle_performance_overlay = 77,
	toggle_foveated_streaming = 79,
	disconnect = 81,
	request_cursor = 82,
	wake_up_computer = 90,
	computer_awaken = 91,
	log = 100,
};
// VD-FACT(games-list): the headset's Games tab. RequestGames (payload: optional u8, non-zero = rescan the library)
// is answered with Games: BE i16 count, then per game u8 GamePlatform + BE u64 id (the Steam app id) + u8 running
// (1 while the Streamer started it and it has not exited). No names: the Streamer sends none; the headset resolves
// Steam titles and art itself and asks for thumbnails (RequestGameThumbnails: BE i16 n + n x (u8 platform, BE u64
// id), each answered by GameThumbnail: platform, id, BE i32 length, JPEG; optional, we send none).
// LaunchGame / ExitGame: u8 platform + BE u64 id. GameExited is never sent by the Streamer.
// TODO(vd-live:games-names): that the headset names an entry with no thumbnail from the Steam app id alone.
enum class GamePlatform : uint8_t
{
	oculus_rift,
	steam,
	viveport,
};
inline constexpr std::size_t games_entry_size = 10;
// VD-FACT(settings-payload): reload = the whole object as compact UTF-8 JSON (no BOM, no length); changed = a JSON
// object with the one member that changed, {"Name":value}. Enums are numbers. The Streamer owns the streamer object
// (sends its reload on connect); the headset owns the user and mobile objects.

// --- Settings keys: VD-FACT(settings-keys) ---------------------------------------------------------------------------
// mobile (headset-owned)
inline constexpr const char * key_hmd_type = "HmdType";
inline constexpr const char * key_ipd = "IPD"; // metres, default 0.0635
inline constexpr const char * key_fov_left = "FovLeft";   // {"UpTan","DownTan","LeftTan","RightTan"}
inline constexpr const char * key_fov_right = "FovRight";
inline constexpr const char * key_spacewarp = "UseSpacewarp";
inline constexpr const char * key_annexb = "UseAnnexB"; // default true
inline constexpr const char * key_foveated = "FoveatedStreaming";
inline constexpr const char * key_features = "Features";
inline constexpr const char * key_measured_bandwidth = "MeasuredBandwidth"; // bit/s
inline constexpr const char * key_refresh_rate = "RefreshRate";
// user (headset-owned)
inline constexpr const char * key_framerate = "VRFramerate"; // 0 = default (72 on Quest-class headsets, else 90)
inline constexpr const char * key_quality = "VRGraphicsQuality"; // default Medium
inline constexpr const char * key_mic_passthrough = "MicPassthrough";
// streamer (ours)
inline constexpr const char * key_has_vr = "HasVR";
inline constexpr const char * key_streaming_source = "StreamingSource";
inline constexpr const char * key_active_codec = "ActiveCodec";
inline constexpr const char * key_allow_extrapolation = "AllowMotionExtrapolation"; // default true
inline constexpr const char * key_auto_bitrate = "AutoAdjustBitrate";
inline constexpr const char * key_can_launch_steamvr = "CanLaunchSteamVR"; // default true

enum class HmdType : int32_t
{
	quest = 259,
	quest2 = 320,
	quest_pro = 400,
	quest3 = 1000,
	quest3s = 1001,
};
enum class Quality : int32_t
{
	potato = -1,
	low,
	medium,
	high,
	ultra,
	godlike,
	monster,
	rampage,
	unstoppable,
};
// Headset capability flags (mobile "Features").
enum Feature : uint32_t
{
	feature_passthrough = 0x2,
	feature_app_spacewarp = 0x4,
	feature_hand_tracking = 0x20,
	feature_face_tracking = 0x40,
	feature_eye_tracking = 0x80,
	feature_body_tracking = 0x100,
	feature_av1 = 0x4000,
	feature_hevc = 0x40000,
};

// --- Video channel: VD-FACT(video-tags, enum-values, eye-delivery, annexb-default, ssw-cadence) ---------------------
// 1.34.25 split the format message (VirtualDesktop.Net.VideoFormat / VideoFormatOld): tag 4 carries 4 stream slots
// (112 bytes, QuaternaryFrameSize/Bounds added), tag 2 the older 3 slots (88 bytes). The Streamer sends the tag-2
// form to clients that predate the split; a 1.34.25 client did not start decoding on it (see vd_wire.cpp).
inline constexpr uint8_t tag_frame = 0, tag_slice = 1, tag_video_format_old = 2, tag_subtitle = 3, tag_video_format = 4;
inline constexpr std::size_t video_format_size = 112, video_format_old_size = 88;
enum class Source : uint8_t
{
	desktop = 0,
	vr = 1,
	video = 2,
	local_video = 3,
};
enum class VideoCodec : int32_t
{
	automatic = 0,
	h264 = 1,
	hevc = 2,
	vp8 = 3,
	vp9 = 4,
	h264_plus = 5,
	hevc_10bit = 6,
	av1 = 10,
	av1_10bit = 11,
};
// Per frame: stream 0 (left eye) rides the frame message, stream 1 (right eye) follows as a slice with the same
// timestamp. Annex-B start codes stay unless the headset turned UseAnnexB off (then 4-byte BE length prefixes).
// TODO(vd-live:annexb-default): the polarity of the writer's flag is inferred (default UseAnnexB=true = raw Annex-B).
// The format's frame rate is the headset display rate (VRFramerate), not halved for SSW; SSW has no signalling.

// VD-FACT(preset-sizes): what the official Streamer uses for a Quest 3 / 3S per quality preset, per eye: the size
// the game renders at and the smaller size it encodes and streams (the headset upscales). Index = Quality + 1.
struct EyeSize
{
	uint32_t width, height;
};
inline constexpr EyeSize quest3_render[9] = {{1344, 1440}, {1728, 1824}, {2112, 2304}, {2496, 2688}, {2688, 2880},
                                             {3072, 3264}, {3648, 4032}, {4224, 4608}, {4992, 5376}};
inline constexpr EyeSize quest3_stream[9] = {{896, 960},   {1152, 1216}, {1408, 1536}, {1664, 1792}, {1792, 1920},
                                             {2048, 2176}, {2432, 2688}, {2816, 3072}, {3328, 3584}};
// VD-FACT(bitrate): Quest 3 ceiling, HEVC/AV1 200 Mbit/s (H.264+ 500); halved while the headset has SSW off.
inline constexpr uint32_t max_bitrate_bps = 200'000'000;
// VD-FACT(fov, ipd): the headset reports FovLeft/FovRight and IPD in its mobile settings; these are used until then.
inline constexpr float default_ipd_m = 0.0635f;

// --- Data channel: VD-FACT(framedata-prefix, framedata-layout, button-bits, clock, aim) -----------------------------
// LE i32 compressed size, LE u64 XXH3-64 (seed 0) of the decompressed bytes, raw LZ4 block. The decompressed struct
// is truncated to the headset's feature set: base 312 bytes, + hands 3344, + body 4632, + eye 4700, + face 4992.
inline constexpr std::size_t frame_data_prefix = 12;
inline constexpr std::size_t frame_data_base = 312, frame_data_hands = 3344, frame_data_body = 4632,
                             frame_data_eye = 4700, frame_data_face = 4992;
// One bitfield for both controllers (i16).
enum Button : uint16_t
{
	button_a = 0x1,
	button_b = 0x2,
	button_x = 0x4,
	button_y = 0x8,
	button_right_thumb = 0x10,
	button_right_trigger = 0x20,
	button_right_grip = 0x40,
	button_left_thumb = 0x80,
	button_left_trigger = 0x100,
	button_left_grip = 0x200,
	button_enter = 0x400, // menu (left controller)
	button_back = 0x800,
};
enum Touch : uint16_t
{
	touch_a = 0x1,
	touch_b = 0x2,
	touch_x = 0x4,
	touch_y = 0x8,
	touch_right_thumb = 0x10,
	touch_right_trigger = 0x20,
	touch_right_thumbrest = 0x40,
	touch_left_thumb = 0x80,
	touch_left_trigger = 0x100,
	touch_left_thumbrest = 0x200,
	touch_right_index_pointing = 0x400,
	touch_right_thumb_up = 0x800,
	touch_left_index_pointing = 0x1000,
	touch_left_thumb_up = 0x2000,
};
// Controllers carry one (grip) pose; there is no aim pose on the wire for controllers.
// TODO(vd-live:aim): aim = grip; whether the headset's grip pose already includes the aim offset the games expect.
// There is no clock-sync message: frame timestamps are host microseconds (opaque to the headset, they pair frame and
// slice); FrameMetadata.frame_index echoes the FrameData.frame_index of the tracking sample the frame was rendered
// with. TODO(vd-live:clock): whether the headset's poses are already predicted to display time (we use them as is).
// FrameData also carries the floor height (m); poses are passed on unchanged (stage space as the headset sends them).

// --- Audio channel: VD-FACT(audio-format). LE i32 byte count + bytes, both directions. -------------------------------
inline constexpr uint32_t audio_rate = 48000, speaker_channels = 2, mic_channels = 1;
inline constexpr uint32_t speaker_packet_bytes = 1152; // 288 stereo S16 frames = 6 ms
} // namespace vrhost::vd
