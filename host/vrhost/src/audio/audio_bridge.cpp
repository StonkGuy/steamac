// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/audio_bridge.h"

#include <chrono>
#include <cstdio>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

namespace vrhost
{
static uint32_t load32(const uint32_t & v)
{
	return std::atomic_ref<uint32_t>(const_cast<uint32_t &>(v)).load(std::memory_order_acquire);
}

vra_region * AudioBridge::map(int fd)
{
	struct stat st{};
	if (fd < 0 or fstat(fd, &st) < 0 or st.st_size < off_t(VRA_REGION_BYTES))
	{
		fprintf(stderr, "steamac-vrhost: --audio-fd is not a %zu-byte audio region\n", size_t(VRA_REGION_BYTES));
		return nullptr;
	}
	void * p = mmap(nullptr, VRA_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
	{
		perror("steamac-vrhost: mmap --audio-fd");
		return nullptr;
	}
	auto * r = static_cast<vra_region *>(p);
	if (not vra_region_valid(r))
	{
		fprintf(stderr, "steamac-vrhost: --audio-fd is not a VR audio region (vraudio.h version %u)\n", VRA_VERSION);
		munmap(p, VRA_REGION_BYTES);
		return nullptr;
	}
	return r;
}

AudioBridge::AudioBridge(vra_region * region, VrSession & s, SessionListener * n, bool run_thread) :
        r(region), session(s), next(n)
{
	if (not run_thread)
		return;
	tick = std::thread([this] {
		auto t = std::chrono::steady_clock::now();
		while (not quit)
		{
			t += std::chrono::milliseconds(quantum_ms);
			auto now = std::chrono::steady_clock::now();
			if (t < now - std::chrono::milliseconds(50))
				t = now; // we were suspended: do not catch up in a burst
			std::this_thread::sleep_until(t);
			publish_headset();
			pump_speaker();
			poll_microphone_state();
		}
	});
}

AudioBridge::~AudioBridge()
{
	quit = true;
	if (tick.joinable())
		tick.join();
	// The guest's audio goes back to the Mac at once, not after the beat timeout.
	std::atomic_ref<uint32_t>(r->headset).store(0, std::memory_order_release);
}

void AudioBridge::publish_headset()
{
	uint32_t flags = 0;
	if (session.connected())
	{
		std::lock_guard lock(fmt_mutex);
		flags = (speaker ? VRA_HEADSET_SPEAKER : 0) | (microphone ? VRA_HEADSET_MIC : 0);
	}
	std::atomic_ref<uint32_t>(r->headset).store(flags, std::memory_order_release);
	std::atomic_ref<uint32_t>(r->beat).fetch_add(1, std::memory_order_release);
}

void AudioBridge::on_connected(const HeadsetInfo & h)
{
	{
		std::lock_guard lock(fmt_mutex);
		speaker = h.speaker;
		microphone = h.microphone;
	}
	last_mic_active.store(-1, std::memory_order_release); // re-announce the microphone state on the new connection
	if (next)
		next->on_connected(h);
}

void AudioBridge::on_disconnected()
{
	{
		std::lock_guard lock(fmt_mutex);
		speaker.reset();
		microphone.reset();
	}
	std::atomic_ref<uint32_t>(r->headset).store(0, std::memory_order_release);
	if (next)
		next->on_disconnected();
}

void AudioBridge::on_tracking(std::span<const TrackedPose> p)
{
	if (next)
		next->on_tracking(p);
}

void AudioBridge::on_input(std::span<const InputValue> v)
{
	if (next)
		next->on_input(v);
}

void AudioBridge::on_refresh_rate(float hz)
{
	if (next)
		next->on_refresh_rate(hz);
}

void AudioBridge::pump_speaker()
{
	std::optional<AudioFormat> fmt;
	{
		std::lock_guard lock(fmt_mutex);
		fmt = speaker;
	}
	size_t avail = pcm::ring_available(r->play);
	uint32_t rate = load32(r->play.rate);
	if (not fmt or rate == 0 or not session.connected())
	{
		// Nobody listens: keep the ring empty so a headset that connects starts with fresh audio.
		st.speaker_discarded += pcm::ring_skip(r->play, avail);
		resampler.reset();
		return;
	}
	if (avail > size_t(rate) * max_backlog_ms / 1000)
	{
		size_t keep = size_t(rate) * keep_after_skip_ms / 1000;
		st.speaker_discarded += pcm::ring_skip(r->play, avail - keep);
		avail = keep;
	}
	if (avail == 0)
		return;
	in.resize(avail * VRA_PLAY_CHANNELS);
	size_t n = pcm::ring_read(r->play, r->play_data, in.data(), avail);
	mixed.resize(n * fmt->channels);
	pcm::remix(in.data(), VRA_PLAY_CHANNELS, mixed.data(), fmt->channels, n);
	out.clear();
	resampler.configure(rate, fmt->rate, fmt->channels);
	resampler.process(mixed, out);

	const size_t quantum = size_t(fmt->rate) * quantum_ms / 1000 * fmt->channels;
	for (size_t off = 0; off < out.size(); off += quantum)
	{
		size_t len = std::min(quantum, out.size() - off);
		session.send_audio(std::span<const int16_t>(out.data() + off, len));
		st.speaker_packets++;
	}
	st.speaker_frames += out.size() / fmt->channels;
}

void AudioBridge::poll_microphone_state()
{
	int active = load32(r->mic.active) != 0;
	int previous = last_mic_active.exchange(active, std::memory_order_acq_rel);
	if (active == previous)
		return;
	session.set_microphone(active);
	st.mic_switches++;
}

void AudioBridge::on_microphone(std::span<const int16_t> pcm, ns time)
{
	std::optional<AudioFormat> fmt;
	{
		std::lock_guard lock(fmt_mutex);
		fmt = microphone;
	}
	if (next)
		next->on_microphone(pcm, time);
	if (not fmt or fmt->channels == 0)
		return;
	size_t frames = pcm.size() / fmt->channels;
	if (frames == 0)
		return;
	// The ring carries the headset's own rate (libkrun converts to the guest's); only the channels change.
	std::atomic_ref<uint32_t>(r->mic.rate).store(fmt->rate, std::memory_order_release);
	thread_local std::vector<int16_t> mono;
	mono.resize(frames);
	pcm::remix(pcm.data(), fmt->channels, mono.data(), VRA_MIC_CHANNELS, frames);
	size_t n = pcm::ring_write(r->mic, r->mic_data, mono.data(), frames);
	st.mic_frames += n;
	st.mic_dropped += frames - n;
}
} // namespace vrhost
