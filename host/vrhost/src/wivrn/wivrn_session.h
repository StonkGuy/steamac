// SPDX-License-Identifier: GPL-3.0-or-later
// WiVRn adapter: a minimal WiVRn-compatible server (protocol of WiVRn v26.9, see third_party/wivrn/UPSTREAM.md)
// behind the VrSession interface. Replaces WiVRn's Monado-bound server/driver with what a stream needs:
// handshake (wivrn_connection.cpp), timesync (clock_offset.cpp), tracking_control, pose history, IDR handling
// (idr_handler.cpp), shard/timing logic (video_encoder.cpp SendData).
#pragma once

#include "session/pose_history.h"
#include "session/vr_session.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vrhost
{
struct WivrnConfig
{
	int port = 9757;      // WiVRn default_port (TCP control + UDP stream)
	bool advertise = true; // Bonjour _wivrn._tcp
	bool tcp_only = false;
	// WiVRn's encryption_state: disabled (plain LAN), enabled (paired headsets only), pairing (paired headsets, or a
	// new one that proves the PIN with SMP). Encryption is X448 + PBKDF2(PIN) + AES-128-CTR (WiVRn common/ code).
	enum class Encryption
	{
		disabled,
		enabled,
		pairing,
	} encryption = Encryption::pairing;
	std::string pin;                 // pairing PIN; empty = random 6 digits (WivrnSession::pin())
	std::string known_keys_file;     // empty = KnownKeys::default_dir()/known_keys
	uint32_t bitrate_bps = 50'000'000;
	// 0 = use the headset's stream_eye_width/height
	uint32_t width = 0, height = 0;
	// An unpaired headset that connects while pairing is off opens pairing with a fresh PIN (on_pin) and is asked for
	// it, instead of being refused (WiVRn refuses; its dashboard then offers "enable pairing"). The PIN is still
	// checked by WiVRn's SMP exchange, so nothing pairs without it.
	bool pair_on_request = false;
	// Render scale (Settings > VR "Render resolution"): the game is asked for the headset's render size times this;
	// below 1 the compositor upscales (CompositorOptions), above 1 is supersampling. 1 = the headset's size.
	float render_scale = 1;
	// Encoder options (Settings > VR "Video codec"): see EncoderConfig.
	float keyframe_interval_s = 0; // > 0: an IDR at least this often (else only when the headset reports a loss)
	bool prioritize_speed = false;
	// Video codec: automatic = HEVC when the headset lists it, else H.264 (WiVRn's default choice for these clients).
	enum class Codec
	{
		automatic,
		hevc,
		h264,
	} codec = Codec::automatic;
	// The headset's app launcher (WiVRn application_list / start_app): id + name. Start requests are only counted
	// (WivrnSession::app_request) for whoever can start the app (the launcher, through the control socket).
	std::vector<std::pair<std::string, std::string>> applications;
	std::function<void(const std::string & pin)> on_pin = nullptr; // pairing PIN changed ("" = pairing off)
	std::function<void(const std::string &)> log = nullptr;
};

// Server-side statistics, for tests and the self-test report.
struct WivrnStats
{
	std::atomic<uint64_t> paired{0}, frames_sent{0}, idr_sent{0}, idr_requests{0}, feedback{0}, tracking{0}, timesync{0},
	        bytes_sent{0}, audio_packets{0}, mic_packets{0}, haptics{0};
	std::atomic<int64_t> last_encode_ns{0};
	std::atomic<int64_t> last_feedback_decode_ns{0}; // received_from_decoder - received_first_packet, headset clock
};

class WivrnSession final : public VrSession
{
public:
	explicit WivrnSession(WivrnConfig);
	~WivrnSession() override;

	void start(SessionListener *) override;
	void stop() override;
	bool connected() const override;
	std::optional<HeadsetInfo> headset() const override;
	FrameTiming next_frame_timing() override;
	bool views_at(ns t, std::array<Pose, 2> & pose, std::array<Fov, 2> & fov) override;
	std::optional<TrackedPose> pose_at(Device, ns t) override;
	void submit(const Frame &) override;
	void haptics(Device, float duration_s, float frequency, float amplitude) override;
	void request_idr() override;
	void send_audio(std::span<const int16_t> pcm) override;
	void set_microphone(bool on) override;

	int port() const;
	std::string pin() const; // the pairing PIN ("" unless encryption == pairing)
	WivrnConfig::Encryption encryption() const;
	// Pairing on (new PIN) / off at run time, for the control socket; false in encryption-disabled mode.
	bool set_pairing(bool on);
	std::string advertised_name() const; // Bonjour service name ("" when not advertised)
	uint32_t bitrate_bps() const;        // configured total bitrate
	std::string known_keys_file() const; // paired-headset store this session uses
	// Total bitrate at run time (control socket "bitrate"): the running encoders and the next connections.
	void set_bitrate(uint32_t bps);
	// Asks the connected headset to switch its display refresh rate (WiVRn refresh_rate_change); false when no
	// headset is connected or it does not offer that rate.
	bool request_refresh_rate(float hz);
	float refresh_rate() const;      // current headset refresh rate (0 = not connected)
	std::string codec_name() const;  // "hevc" | "h264" while connected, else ""
	struct Battery
	{
		float charge; // 0..1
		bool charging;
	};
	std::optional<Battery> battery() const; // last battery packet of the connected headset
	struct PairingRequest
	{
		std::string name;
		int64_t at = 0;       // unix seconds
		bool refused = false; // pairing was off and pair_on_request too
	};
	std::optional<PairingRequest> pairing_request() const; // last unpaired headset that connected
	struct AppRequest
	{
		std::string id;
		uint64_t seq = 0; // counts start_app requests; 0 = none yet
	};
	AppRequest app_request() const;
	const WivrnStats & stats() const;
	static uint64_t protocol_version();

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace vrhost
