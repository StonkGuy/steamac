// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: a LAN-only stand-in for the VD Streamer's VR mode behind the VrSession interface, so the
// user's own VD headset client can stream from this Mac. Own implementation from private interoperability evidence
// (personal use; this adapter lives on a private branch only). Protocol facts: vd_assumptions.h.
//
//   UDP discovery (38850): offer (RSA key + account) -> RSA(wrapper key) + record (connection id, channel key/IV)
//                          then "connect now" (connection id)
//   TCP control 38810 (always encrypted): settings sync (streamer out; user + mobile in, JSON), ForceKeyFrame,
//                          RestartStream, Recenter, UpdateRequired, WakeUp, Disconnect/Exit; host commands out
//   TCP video   38830: VideoFormatOld, then per frame: FrameMetadata (pose, timing, haptics) + eye 0, slice eye 1
//   TCP audio   38840: speaker PCM down, microphone PCM up
//   TCP data    38820: bandwidth probe down; tracking + input up (LZ4 + XXH3), per headset frame
//
// The discovery key is a constant of the official apps. It is never part of this tree: VdConfig::discovery_key is
// filled at run time from the user's own installed Streamer (vd_streamer_key.h), never logged or stored.
#pragma once

#include "session/session_control.h"
#include "session/vr_session.h"
#include "vd/vd_assumptions.h"
#include "vd/vd_crypto.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vrhost
{
struct VdConfig
{
	// 0 = ephemeral (tests); the computer record carries the bound ports.
	uint16_t control_port = 38810, data_port = 38820, video_port = 38830, audio_port = 38840, discovery_port = 38850;
	std::optional<vd::AesKey> discovery_key; // nullopt: no discovery responder (and no encryption)
	bool encrypt_local = true;               // AES stream wrapper on the LAN (needs discovery_key)
	std::string accounts_file;               // empty = KnownKeys::default_dir()/vd_accounts
	bool pairing = false;                    // allow a new account now (also when none is allowed yet)
	std::string computer_name;               // empty = host name
	std::vector<std::string> addresses;      // advertised private addresses; empty = this Mac's interfaces
	uint32_t bitrate_bps = 100'000'000;      // total for both eyes, capped at vd::max_bitrate_bps
	uint32_t width = 0, height = 0;          // per-eye stream size; 0 = the headset's quality preset
	enum class CodecChoice
	{
		automatic, // HEVC when the headset decodes it, else H.264
		h264,
		hevc,
	} codec = CodecChoice::automatic;
	enum class Spacewarp
	{
		headset, // render divisor 2 while the headset's UseSpacewarp setting is on
		off,
		on,
	} spacewarp = Spacewarp::headset;
	// The game renders at render_scale x VD's render size (1.5x the stream); below 1 the compositor upscales to the
	// render size before the foveated encoding. Default 2/3: the game renders at the stream size (the GPU load of
	// the adapter before the encoding fix; the centre is upscaled instead of 1:1). 1 = VD's own quality.
	float render_scale = 2.f / 3;
	ns latency_ns = 30'000'000; // wake-up to display (render + encode + network + decode), for predicted_display
	std::function<void(const std::string &)> log = nullptr;
};

struct VdStats
{
	std::atomic<uint64_t> offers{0}, offers_refused{0}, connect_now{0}, sessions{0}, frames_sent{0}, idr_sent{0},
	        idr_requests{0}, bytes_sent{0}, tracking{0}, inputs{0}, audio_packets{0}, mic_packets{0}, haptics{0},
	        settings_messages{0}, bad_messages{0}, recenters{0}, commands_sent{0}, frames_dropped{0};
	std::atomic<int32_t> last_latency_us{0}; // headset-reported latency (FrameData)
	std::atomic<int64_t> last_encode_ns{0};
};

class VdSession final : public VrSession
{
public:
	explicit VdSession(VdConfig);
	~VdSession() override;

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

	struct Ports
	{
		uint16_t control, data, video, audio, discovery;
	};
	Ports ports() const; // bound ports (valid after start)
	uint32_t render_divisor() const;
	bool set_pairing(bool on);
	bool pairing() const;
	std::string accounts_file() const;
	uint32_t bitrate_bps() const;
	const VdStats & stats() const;
	// Streamer-side settings the host publishes (JSON), and a change of one (sent to the headset).
	std::string streamer_settings() const;
	void set_streamer_setting(const std::string & key, const std::string & json_value);
	// Host-side commands the Streamer sends to the headset (passthrough, performance overlay, foveated toggle).
	bool send_command(vd::Msg);

private:
	struct impl;
	std::unique_ptr<impl> p;
};

class VdControl final : public SessionControl
{
public:
	explicit VdControl(VdSession & s) : s(s) {}
	VrSession & session() override { return s; }
	std::string protocol() const override { return "vd"; }
	PairingState pairing_state() const override;
	std::vector<Param> params() const override;
	bool set_pairing(bool on) override { return s.set_pairing(on); }
	std::vector<Peer> paired() const override;
	int unpair(const std::string & name) override;
	uint32_t bitrate_bps() const override { return s.bitrate_bps(); }
	Counters counters() const override;

private:
	VdSession & s;
};
} // namespace vrhost
