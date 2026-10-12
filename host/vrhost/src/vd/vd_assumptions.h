// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: every protocol detail the interoperability notes (PROTOCOL.md, local and private) leave
// open is decided HERE, under a TODO(vd-live:<name>) that the first trial against the user's own headset (M-vd4)
// must confirm or correct. tests/test_vd_wire.cpp pins each value, so changing one is a deliberate, visible edit.
// Own code; no vendor code, tables, keys or constants (the discovery key is supplied at run time, see vd_session.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace vrhost::vd
{
// --- Ports (PROTOCOL.md section 1, observed in the reference capture) -------------------------------------------
inline constexpr uint16_t default_control_port = 38810;
inline constexpr uint16_t default_data_port = 38820;
inline constexpr uint16_t default_video_port = 38830;
inline constexpr uint16_t default_audio_port = 38840;
inline constexpr uint16_t default_discovery_port = 38850;

// Version string reported in the computer record. TODO(vd-live:version): whether the LAN client checks it.
inline constexpr const char * streamer_version = "1.34.22";

// --- Discovery (PROTOCOL.md section 2) -------------------------------------------------------------------------------
// TODO(vd-live:discovery-cipher): every datagram is AES-256-CBC with PKCS#7 padding under the shared key+IV, each
// datagram on its own (no chaining between datagrams).
// TODO(vd-live:connect-now): "connect now" = 16-byte connection GUID + 1-byte platform id, after unwrapping.
inline constexpr std::size_t connect_now_size = 17;
// TODO(vd-live:offer-layout): an offer is the RSA public key as .NET XML text (<RSAKeyValue>..</RSAKeyValue>),
// then optionally a 2-byte big-endian platform id and the UTF-8 account id up to the end of the datagram.
inline constexpr const char * rsa_key_end = "</RSAKeyValue>";
// TODO(vd-live:rsa-padding): the session key (32 B) + IV (16 B) are RSA-encrypted with PKCS#1 v1.5 padding.
// TODO(vd-live:reply-layout): reply = RSA block (modulus size) followed by the discovery-wrapped computer record.
// TODO(vd-live:record-schema): the record is our own DataContract-style XML (vd_discovery.cpp make_record):
// element names, the OS value text and how ports/addresses are listed are guesses. The session key is NOT repeated
// in the LAN record (only in the RSA block).
inline constexpr const char * os_windows = "Windows"; // user decision: the adapter reports Windows

// --- Stream wrapper (PROTOCOL.md section 3) --------------------------------------------------------------------------
// TODO(vd-live:stream-cipher): AES-256-CBC with the session key; one continuous CBC chain per channel and direction,
// each starting from the session IV. Chunk = 7-bit varint length + payload, random padding to 16 bytes.
// TODO(vd-live:plain-framing): without encryption the wrapper is transparent (no varint): every message on every
// channel is self-delimiting (control header length, frame/slice data_size, data/audio length prefixes).
// TODO(vd-live:peer-info): on the control channel the headset sends PeerInfo first, the host answers with its own;
// PeerInfo = address (family byte 4|16, address bytes, u32 BE port) + 1 byte encrypt-traffic. Encryption (if the
// host's flag says so) starts after the exchange on control and right after connect on data/video/audio.

// --- Control messages (PROTOCOL.md section 5): 1-byte type, u32 BE payload length, payload ------------------------
// TODO(vd-live:control-ids): the numeric message type values are not in the notes; these are placeholders.
enum class Msg : uint8_t
{
	reload_user_settings = 1,
	reload_mobile_settings = 2,
	reload_streamer_settings = 3,
	user_setting_changed = 4,
	mobile_setting_changed = 5,
	streamer_setting_changed = 6,
	force_key_frame = 10,
	restart_stream = 11,
	recenter = 12,
	disconnect = 20,
	exit = 21,
};
// TODO(vd-live:settings-payload): reload = the whole settings object as UTF-8 JSON (the payload, no string length);
// changed = BE-u16-length name + BE-u16-length JSON value.

// --- Settings keys (PROTOCOL.md sections 8.6, 9) ---------------------------------------------------------------------
// TODO(vd-live:settings-keys): key names spelled as the notes name the settings; HmdType/VideoCodec as enum names.
inline constexpr const char * key_hmd_type = "HmdType";
inline constexpr const char * key_quality = "VRGraphicsQuality";
inline constexpr const char * key_framerate = "Framerate";
inline constexpr const char * key_spacewarp = "UseSpacewarp";
inline constexpr const char * key_annexb = "UseAnnexB";
inline constexpr const char * key_codec = "VideoCodec";
inline constexpr const char * key_bitrate = "Bitrate"; // Mbit/s
inline constexpr const char * key_allow_extrapolation = "AllowMotionExtrapolation";
inline constexpr const char * key_vr_available = "VRAvailable";

// --- Video channel (PROTOCOL.md section 8.1/8.2) --------------------------------------------------------------------
// TODO(vd-live:video-tags): message tags: 0 frame, 1 slice (both from the notes), 2 video format (placeholder).
inline constexpr uint8_t tag_frame = 0, tag_slice = 1, tag_video_format = 2;
// TODO(vd-live:enum-values): source/codec values follow the order the notes list them in.
enum class Source : int32_t
{
	desktop = 0,
	vr = 1,
	video = 2,
};
enum class VideoCodec : int32_t
{
	h264 = 0,
	h264_plus = 1,
	hevc = 2,
	hevc_10bit = 3,
	av1 = 4,
	av1_10bit = 5,
};
// TODO(vd-live:annexb-default): without a UseAnnexB setting the host sends 4-byte BE length-prefixed NALs.
// TODO(vd-live:eye-delivery): per frame, eye 0 rides the frame message (tag 0, FrameMetadata.data_size = eye 0
// bytes) and eye 1 follows as a slice (tag 1, stream index 1) with the same timestamp.

// Quest 3 floor, user fact: VD "Low" ≈ 1700x1700 per eye. TODO(vd-live:preset-sizes): our own preset table beyond it.
inline constexpr uint32_t quest3_low_eye = 1700;
// VD caps the bitrate at 200 Mbit/s (user + reference capture).
inline constexpr uint32_t max_bitrate_bps = 200'000'000;
// TODO(vd-live:fov): the headset FOV is not in any message the notes describe; Quest 3-like tangents until measured.
// TODO(vd-live:ipd): IPD likewise; 64 mm.
inline constexpr float default_ipd_m = 0.064f;

// --- Data channel (PROTOCOL.md section 8.4) --------------------------------------------------------------------------
// TODO(vd-live:framedata-prefix): i32 LE compressed size, i32 LE raw size, u64 LE XXH3-64 of the raw bytes, then the
// LZ4 block. TODO(vd-live:framedata-layout): base FrameData layout in vd_wire.h; feature blocks are ignored.
// TODO(vd-live:button-bits): button/touch bit order = the order the notes list them.
enum Button : uint32_t
{
	button_a = 1u << 0,
	button_b = 1u << 1,
	button_x = 1u << 2,
	button_y = 1u << 3,
	button_thumb = 1u << 4,
	button_trigger = 1u << 5,
	button_grip = 1u << 6,
	button_thumbrest = 1u << 7,
	button_index_pointing = 1u << 8,
	button_thumb_up = 1u << 9,
	button_enter = 1u << 10, // menu (left)
	button_back = 1u << 11,  // system (right)
};

// --- Audio channel (PROTOCOL.md section 1; capture: ~1.65 Mbit/s = 48 kHz stereo S16 + overhead) -------------------
// TODO(vd-live:audio-format): i32 LE byte count + interleaved S16 LE PCM; speaker 48 kHz stereo, microphone 48 kHz
// mono, microphone always on (no request message in the notes).
inline constexpr uint32_t audio_rate = 48000, speaker_channels = 2, mic_channels = 1;
} // namespace vrhost::vd
