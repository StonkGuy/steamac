// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only headset that follows the WiVRn v26.9 client connection sequence (client/wivrn_client.cpp, encryption
// disabled, or PIN pairing + AES-128-CTR with WiVRn's own crypto/smp/secrets code) over loopback, sends headset info / tracking / timesync answers / feedback, and reassembles and decodes the
// video shards with VideoToolbox. The end-to-end check against the real WiVRn client is done separately (docs/vr.md).
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct FakeHeadsetOptions
{
	int port = 9757;
	// Server address. An IPv4 literal connects like the WiVRn client does for an IPv4 server address (AF_INET control
	// and stream sockets); the dual-stack server then sees a v4-mapped peer. Default: IPv6 loopback.
	std::string host = "::1";
	int pin_delay_ms = 0; // the "user" types the PIN this long after the PIN prompt (the client polls the socket meanwhile)
	uint64_t protocol_version = 0; // 0 = ours
	int frames = 60;              // decoded frames wanted per eye
	int64_t drop_frame = -1;      // ignore all shards of this frame on stream 0 and report it lost
	uint16_t eye_width = 640, eye_height = 704;
	int timeout_ms = 20000;
	int64_t clock_offset_ns = 5'000'000'000; // headset clock = host clock + this
	std::string private_key; // headset X448 key (PEM); empty = a new one each run
	std::string pin = "000000"; // what the "user" types when the server asks for a PIN
	// Synthetic headset timing for the pacer: the headset's decoder finishes this long after the frame's first shard,
	// and blits/displays on a vsync grid of display_period_ns (phase display_phase_ns) in the headset clock.
	int64_t decode_ns = 0;
	int64_t display_period_ns = 0; // 0 = report no blit/display times
	int64_t display_phase_ns = 0;
	bool identity_foveation = true;          // view_info must carry identity foveation (game frames: foveated)
	std::function<void(int eye, uint64_t frame_idx, CVPixelBufferRef)> on_decoded; // optional content check
	// A headset closer to a real one (vrhost-test-headset --quest3): render size the game should use (0 = the stream
	// size, as the loopback tests use), per-eye FOV (left, right, up, down; radians), refresh, head motion
	// (sinusoids of these amplitudes in radians at motion_hz, yaw then pitch phase-shifted) and controllers
	// (grip/aim poses plus a trigger ramp and an A press every second).
	uint16_t render_width = 0, render_height = 0;
	std::array<std::array<float, 4>, 2> fov{{{-0.9f, 0.8f, 0.85f, -0.9f}, {-0.9f, 0.8f, 0.85f, -0.9f}}};
	float refresh_hz = 72;
	float yaw_amplitude = 0.3f, pitch_amplitude = 0, motion_hz = 0.159f;
	bool controllers = false;
	struct View
	{
		std::array<std::array<float, 4>, 2> orientation; // x, y, z, w per eye (view_info pose: the render pose)
		std::array<std::array<float, 3>, 2> position;
		std::array<std::array<float, 4>, 2> fov;
		std::array<std::vector<uint16_t>, 2> foveation_x, foveation_y;
	};
	std::function<void(int eye, uint64_t frame_idx, CVPixelBufferRef, const View &)> on_view; // decoded + its view_info
	// Audio (WiVRn headset_info speaker/microphone). Microphone: once the server enables it (feature_control), send
	// a mic_tone_hz sine (amplitude 8000) in 5 ms packets, with a full-scale marker of 48 frames mic_marker_ms after
	// the first packet.
	uint8_t speaker_channels = 0; // 0 = no speaker
	uint32_t speaker_rate = 48000;
	uint8_t mic_channels = 0; // 0 = no microphone
	uint32_t mic_rate = 48000;
	double mic_tone_hz = 440;
	int mic_marker_ms = 300;
	bool expect_h264 = false;   // the server was told to use H.264 (both codecs are offered either way)
	float battery_charge = -1;  // >= 0: send one battery packet (charging) after the headset info
	std::string start_app;      // non-empty: ask for the app list, then to start this app id
	// Wi-Fi-like conditions on the video stream (UDP shards), applied as they arrive: one-way delay plus uniform
	// jitter (order kept, as a Wi-Fi link keeps it), random loss, a rate cap (bytes queue behind it) and loss
	// bursts (every burst_every_s seconds all shards for burst_ms). 0 = off.
	struct Network
	{
		float delay_ms = 0, jitter_ms = 0, loss_pct = 0, mbps = 0, burst_every_s = 0, burst_ms = 0;
	} net;
};

struct FakeHeadsetResult
{
	std::string error;            // empty on success of the protocol part
	std::string handshake_state;  // to_headset::crypto_handshake state name
	bool encrypted = false;
	std::vector<std::pair<uint64_t, int64_t>> display_times; // (frame index, view_info.display_time) stream 0
	std::vector<std::pair<uint64_t, std::array<float, 4>>> view_orientation; // (frame index, view_info left eye) stream 0
	bool got_stream_description = false;
	bool got_tracking_control = false;
	int timesync_queries = 0;
	std::array<int, 2> decoded{};       // frames decoded per eye
	std::array<int, 2> index_mismatch{}; // decoded frame index != frame_idx
	std::array<int, 2> idr{};            // IDR access units received
	std::array<int, 2> index_gaps{};     // frame indices skipped on the wire (reported lost, as the WiVRn client does)
	int idr_after_drop = 0;              // IDRs on stream 0 after the dropped frame
	int decoded_after_drop = 0;          // stream 0 frames decoded after the drop
	int shards = 0, max_shard_bytes = 0;
	int control_shards = 0; // video shards that came over TCP while a UDP stream was up (WiVRn sends none)
	bool view_info_ok = true;    // first shard carries view_info with identity foveation and plausible poses
	double display_time_offset_ms = 0; // view_info.display_time - (host now + clock_offset), last frame
	std::array<std::vector<uint16_t>, 2> foveation_x, foveation_y; // view_info foveation of the last decoded frame
	// audio
	bool got_audio_description = false; // matches what the headset offered
	std::vector<bool> mic_feature;      // feature_control(microphone) states received, in order
	std::vector<int16_t> speaker;       // all speaker PCM received, interleaved
	int speaker_packets = 0, max_speaker_packet_frames = 0;
	std::vector<std::pair<int64_t, size_t>> speaker_arrivals; // (host ns, frames received so far after it)
	int64_t mic_marker_sent_ns = 0;     // host clock
	size_t mic_frames_sent = 0;
	struct Haptic
	{
		int device;
		int64_t duration_ns;
		float frequency, amplitude;
	};
	std::vector<Haptic> haptics;
	std::vector<std::string> app_ids;      // last application_list received
	std::vector<float> refresh_changes;    // refresh_rate_change requests received (answered with refresh_rate_changed)
	int net_dropped = 0;                   // shards the network shaping dropped
};

FakeHeadsetResult run_fake_headset(const FakeHeadsetOptions &);
