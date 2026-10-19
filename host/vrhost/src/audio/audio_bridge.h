// SPDX-License-Identifier: GPL-3.0-or-later
// Game audio <-> headset (docs/vr-interfaces.md section 6). The guest's playback arrives from libkrun's virtio-snd
// CoreAudio backend in the shared vra_region play ring; every 5 ms (WiVRn's audio quantum) the bridge sends what is
// there to the headset in its speaker format. The headset's microphone goes the other way into the mic ring, and
// the ring's `active` flag (set by libkrun while the guest records) switches the headset microphone on and off.
//
// The buffering policy is WiVRn's (server/audio/audio_pipewire.cpp, client/audio/android/audio.cpp): the server
// sends a quantum as soon as it has it and never pads; the client jitter-buffers (30 ms target) and plays silence
// when it runs dry; on the microphone side excess beyond 80 ms is discarded so latency cannot accumulate. Here the
// libkrun side owns the mic pacing (it fills silence when the headset is late), so the bridge only converts.
//
// The bridge is a SessionListener decorator: pass it to VrSession::start() and it forwards every event to `next`.
#pragma once

#include "audio/pcm.h"
#include "session/vr_session.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace vrhost
{
struct AudioBridgeStats
{
	std::atomic<uint64_t> speaker_frames{0}, speaker_packets{0}, speaker_discarded{0}, mic_frames{0},
	        mic_dropped{0}, mic_switches{0};
};

class AudioBridge final : public SessionListener
{
public:
	static constexpr int quantum_ms = 5;
	static constexpr int max_backlog_ms = 100;
	static constexpr int keep_after_skip_ms = 20;

	// `region` must stay mapped while the bridge lives; `next` may be null. Without `run_thread` nothing ticks.
	AudioBridge(vra_region * region, VrSession & session, SessionListener * next, bool run_thread = true);
	~AudioBridge() override;

	static vra_region * map(int fd);

	void on_connected(const HeadsetInfo &) override;
	void on_disconnected() override;
	void on_tracking(std::span<const TrackedPose> p) override;
	void on_input(std::span<const InputValue> v) override;
	void on_refresh_rate(float hz) override;
	void on_microphone(std::span<const int16_t> pcm, ns time) override;

	void pump_speaker();
	void poll_microphone_state();
	void publish_headset();
	const AudioBridgeStats & stats() const { return st; }

private:
	vra_region * r;
	VrSession & session;
	SessionListener * next;
	AudioBridgeStats st;

	std::mutex fmt_mutex;
	std::optional<AudioFormat> speaker, microphone;

	pcm::Resampler resampler;
	std::vector<int16_t> in, mixed, out;
	std::atomic<int> last_mic_active{-1};

	std::atomic<bool> quit{false};
	std::thread tick;
};
} // namespace vrhost
