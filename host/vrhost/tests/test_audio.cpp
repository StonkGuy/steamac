// SPDX-License-Identifier: GPL-3.0-or-later
// VR audio bridge units: the vra_ring SPSC helpers (wrap, overrun, concurrent producer/consumer), channel remix,
// the streaming resampler (block-split invariance, tone frequency, passthrough), and AudioBridge against a recording
// VrSession: 5 ms packets in the headset's format, stale backlog skipped, nothing sent while disconnected, the
// microphone converted into the mic ring and switched by the ring's `active` flag.
#include "audio/audio_bridge.h"
#include "check.h"

#include <cmath>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace vrhost;

static std::unique_ptr<vra_region> new_region()
{
	auto r = std::make_unique<vra_region>();
	vra_region_init(r.get());
	return r;
}

static std::vector<int16_t> tone(double hz, uint32_t rate, uint32_t ch, size_t frames, size_t start = 0)
{
	std::vector<int16_t> v(frames * ch);
	for (size_t f = 0; f < frames; ++f)
		for (uint32_t c = 0; c < ch; ++c)
			v[f * ch + c] = int16_t(std::lround(10000 * std::sin(2 * M_PI * hz * double(f + start) / rate)));
	return v;
}

// zero crossings (rising) per second
static double frequency(const std::vector<int16_t> & v, uint32_t ch, uint32_t rate)
{
	int rising = 0;
	size_t frames = v.size() / ch, first = 0, last = 0;
	for (size_t f = 1; f < frames; ++f)
		if (v[(f - 1) * ch] < 0 and v[f * ch] >= 0)
		{
			if (not rising)
				first = f;
			last = f;
			rising++;
		}
	return rising > 1 ? double(rising - 1) * rate / double(last - first) : 0;
}

struct RecordingListener final : SessionListener
{
	int connected = 0, disconnected = 0, tracking = 0, inputs = 0, refreshes = 0, microphone_packets = 0;
	void on_connected(const HeadsetInfo &) override { connected++; }
	void on_disconnected() override { disconnected++; }
	void on_tracking(std::span<const TrackedPose>) override { tracking++; }
	void on_input(std::span<const InputValue>) override { inputs++; }
	void on_refresh_rate(float) override { refreshes++; }
	void on_microphone(std::span<const int16_t>, ns) override { microphone_packets++; }
};

struct RecordingSession final : VrSession
{
	bool is_connected = true;
	std::vector<std::vector<int16_t>> packets;
	std::vector<bool> mic;
	void start(SessionListener *) override {}
	void stop() override {}
	bool connected() const override { return is_connected; }
	std::optional<HeadsetInfo> headset() const override { return {}; }
	FrameTiming next_frame_timing() override { return {}; }
	bool views_at(ns, std::array<Pose, 2> &, std::array<Fov, 2> &) override { return false; }
	std::optional<TrackedPose> pose_at(Device, ns) override { return {}; }
	void submit(const Frame &) override {}
	void haptics(Device, float, float, float) override {}
	void request_idr() override {}
	void send_audio(std::span<const int16_t> pcm) override { packets.emplace_back(pcm.begin(), pcm.end()); }
	void set_microphone(bool on) override { mic.push_back(on); }
};

static void test_map()
{
	char path[] = "/tmp/vrhost-audio-short-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd >= 0)
	{
		unlink(path);
		CHECK(AudioBridge::map(fd) == nullptr);
		CHECK(ftruncate(fd, VRA_REGION_BYTES) == 0);
		auto initial = std::make_unique<vra_region>();
		vra_region_init(initial.get());
		CHECK(pwrite(fd, initial.get(), sizeof(*initial), 0) == ssize_t(sizeof(*initial)));
		auto * region = AudioBridge::map(fd);
		CHECK(region != nullptr);
		if (region)
		{
			vra_region_init(region);
			CHECK(vra_region_valid(region));
			munmap(region, VRA_REGION_BYTES);
		}
		close(fd);
	}
	CHECK(AudioBridge::map(-1) == nullptr);
}

static void test_ring()
{
	auto r = new_region();
	CHECK(vra_region_valid(r.get()));
	std::vector<int16_t> src(3000 * 2), dst(8192 * 2);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = int16_t(i);
	// wrap around the 8192-frame ring several times
	size_t total = 0;
	for (int round = 0; round < 10; ++round)
	{
		CHECK(pcm::ring_write(r->play, r->play_data, src.data(), 3000) == 3000);
		CHECK(pcm::ring_available(r->play) == 3000);
		CHECK(pcm::ring_read(r->play, r->play_data, dst.data(), 8192) == 3000);
		CHECK(std::equal(src.begin(), src.end(), dst.begin()));
		total += 3000;
	}
	CHECK(r->play.write == total and r->play.read == total);
	// overrun: only the free part is taken, the rest is counted
	CHECK(pcm::ring_write(r->play, r->play_data, dst.data(), 8192) == 8192);
	CHECK(pcm::ring_write(r->play, r->play_data, src.data(), 100) == 0);
	CHECK(r->play.overruns == 100);
	CHECK(pcm::ring_skip(r->play, 10000) == 8192);

	// concurrent producer/consumer: the sequence arrives intact
	std::atomic<bool> done{false};
	std::thread prod([&] {
		std::vector<int16_t> blk(2 * 333);
		int16_t v = 0;
		for (int i = 0; i < 3000; ++i)
		{
			for (size_t f = 0; f < 333; ++f)
				blk[2 * f] = blk[2 * f + 1] = v++;
			size_t off = 0;
			while (off < 333)
				off += pcm::ring_write(r->play, r->play_data, blk.data() + 2 * off, 333 - off);
		}
		done = true;
	});
	int16_t expect = 0;
	bool ok = true;
	size_t got = 0;
	while (not done or pcm::ring_available(r->play))
	{
		size_t n = pcm::ring_read(r->play, r->play_data, dst.data(), 512);
		for (size_t f = 0; f < n; ++f, ++expect)
			ok &= dst[2 * f] == expect and dst[2 * f + 1] == expect;
		got += n;
	}
	prod.join();
	CHECK(ok);
	CHECK(got == 3000 * 333);
}

static void test_remix()
{
	int16_t st[] = {100, 300, -200, -400};
	int16_t mono[2];
	pcm::remix(st, 2, mono, 1, 2);
	CHECK(mono[0] == 200 and mono[1] == -300);
	int16_t back[4];
	pcm::remix(mono, 1, back, 2, 2);
	CHECK(back[0] == 200 and back[1] == 200 and back[2] == -300 and back[3] == -300);
}

static void test_resampler()
{
	// passthrough
	pcm::Resampler pass;
	pass.configure(48000, 48000, 2);
	std::vector<int16_t> out;
	auto in = tone(440, 48000, 2, 480);
	pass.process(in, out);
	CHECK(out == in);

	// 48 kHz -> 44.1 kHz: frame count follows the ratio, the tone keeps its pitch, and splitting the input into
	// odd blocks gives the same output as one block.
	auto src = tone(1000, 48000, 2, 48000);
	pcm::Resampler one, split;
	one.configure(48000, 44100, 2);
	split.configure(48000, 44100, 2);
	std::vector<int16_t> a, b;
	one.process(src, a);
	for (size_t off = 0; off < 48000;)
	{
		size_t n = std::min<size_t>(48000 - off, 37 + off % 211);
		split.process(std::span<const int16_t>(src.data() + 2 * off, 2 * n), b);
		off += n;
	}
	CHECK(a == b);
	CHECK(std::abs(int(a.size() / 2) - 44100) <= 1);
	double f = frequency(a, 2, 44100);
	printf("resampler 48k->44.1k: %zu frames, tone %.1f Hz\n", a.size() / 2, f);
	CHECK(std::abs(f - 1000) < 2);
	// upsampling 44.1 -> 48
	pcm::Resampler up;
	up.configure(44100, 48000, 1);
	std::vector<int16_t> c;
	up.process(tone(500, 44100, 1, 44100), c);
	CHECK(std::abs(int(c.size()) - 48000) <= 1);
	CHECK(std::abs(frequency(c, 1, 48000) - 500) < 2);
}

static void test_bridge_speaker()
{
	auto r = new_region();
	RecordingSession s;
	AudioBridge bridge(r.get(), s, nullptr, false);
	HeadsetInfo h;
	h.speaker = AudioFormat{1, 48000};
	h.microphone = AudioFormat{1, 48000};
	bridge.on_connected(h);
	r->play.rate = 48000;

	// 12 ms of stereo -> 3 packets (5, 5, 2 ms) in mono
	auto src = tone(440, 48000, 2, 576);
	pcm::ring_write(r->play, r->play_data, src.data(), 576);
	bridge.pump_speaker();
	CHECK(s.packets.size() == 3);
	CHECK(s.packets.size() == 3 and s.packets[0].size() == 240 and s.packets[1].size() == 240 and s.packets[2].size() == 96);
	CHECK(s.packets.size() >= 1 and s.packets[0][10] == src[20]);
	CHECK(pcm::ring_available(r->play) == 0);

	// a stale backlog (300 ms) is cut to the last 20 ms
	s.packets.clear();
	auto big = tone(440, 48000, 2, 14400);
	pcm::ring_write(r->play, r->play_data, big.data(), 8000);
	bridge.pump_speaker();
	size_t frames = 0;
	for (auto & p: s.packets)
		frames += p.size();
	CHECK(frames == 960);
	CHECK(bridge.stats().speaker_discarded == 8000 - 960);

	// disconnected: drained and dropped, nothing sent
	s.packets.clear();
	s.is_connected = false;
	pcm::ring_write(r->play, r->play_data, big.data(), 480);
	bridge.pump_speaker();
	CHECK(s.packets.empty());
	CHECK(pcm::ring_available(r->play) == 0);

	// a 44.1 kHz guest stream to a 48 kHz stereo headset
	s.is_connected = true;
	h.speaker = AudioFormat{2, 48000};
	bridge.on_connected(h);
	r->play.rate = 44100;
	auto g = tone(1000, 44100, 2, 4410);
	std::vector<int16_t> all;
	for (size_t off = 0; off < 4410; off += 441)
	{
		pcm::ring_write(r->play, r->play_data, g.data() + 2 * off, 441);
		s.packets.clear();
		bridge.pump_speaker();
		for (auto & p: s.packets)
		{
			CHECK(p.size() <= 480);
			all.insert(all.end(), p.begin(), p.end());
		}
	}
	CHECK(std::abs(int(all.size() / 2) - 4800) <= 2);
	CHECK(std::abs(frequency(all, 2, 48000) - 1000) < 5);
}

static void test_bridge_microphone()
{
	auto r = new_region();
	RecordingSession s;
	AudioBridge bridge(r.get(), s, nullptr, false);
	HeadsetInfo h;
	h.microphone = AudioFormat{2, 44100}; // a headset reporting stereo: averaged to the mono ring
	bridge.on_connected(h);

	bridge.poll_microphone_state();
	CHECK(s.mic == std::vector<bool>{false});
	r->mic.active = 1; // libkrun: the guest started recording
	bridge.poll_microphone_state();
	bridge.poll_microphone_state();
	CHECK((s.mic == std::vector<bool>{false, true}));

	int16_t pcm[] = {1000, 3000, -1000, -3000};
	bridge.on_microphone(pcm, 0);
	CHECK(r->mic.rate == 44100);
	CHECK(pcm::ring_available(r->mic) == 2);
	int16_t got[2];
	pcm::ring_read(r->mic, r->mic_data, got, 2);
	CHECK(got[0] == 2000 and got[1] == -2000);

	// a full mic ring drops (libkrun not reading) and counts
	std::vector<int16_t> lots(2 * 9000, 7);
	bridge.on_microphone(lots, 0);
	CHECK(bridge.stats().mic_dropped == 9000 - VRA_MIC_FRAMES);
	CHECK(r->mic.overruns == 9000 - VRA_MIC_FRAMES);

	// reconnect: state announced again
	bridge.on_connected(h);
	bridge.poll_microphone_state();
	CHECK((s.mic == std::vector<bool>{false, true, true}));
}

// vra_region.headset follows the connected headset and beat moves on every tick; gone at once on disconnect and
// when the bridge goes away (libkrun then routes the guest's audio to the Mac again).
static void test_bridge_headset_state()
{
	auto r = new_region();
	RecordingSession s;
	RecordingListener next;
	s.is_connected = false;
	{
		AudioBridge bridge(r.get(), s, &next, false);
		bridge.publish_headset();
		CHECK(r->headset == 0 and r->beat == 1);
		HeadsetInfo h;
		h.speaker = AudioFormat{2, 48000};
		s.is_connected = true;
		bridge.on_connected(h);
		bridge.publish_headset();
		TrackedPose pose{};
		InputValue input{};
		bridge.on_tracking(std::span<const TrackedPose>(&pose, 1));
		bridge.on_input(std::span<const InputValue>(&input, 1));
		bridge.on_refresh_rate(90.0f);
		int16_t mic_pcm[] = {1, 2};
		bridge.on_microphone(mic_pcm, 123);
		CHECK(next.connected == 1 and next.tracking == 1 and next.inputs == 1 and next.refreshes == 1);
		CHECK(next.microphone_packets == 1);
		CHECK(r->headset == VRA_HEADSET_SPEAKER and r->beat == 2);
		h.microphone = AudioFormat{1, 48000};
		bridge.on_connected(h);
		bridge.publish_headset();
		CHECK(r->headset == (VRA_HEADSET_SPEAKER | VRA_HEADSET_MIC));
		bridge.on_disconnected();
		CHECK(r->headset == 0);
		bridge.on_connected(h);
		bridge.publish_headset();
		CHECK(r->headset != 0);
	}
	CHECK(r->headset == 0);
}

int main()
{
	test_map();
	test_ring();
	test_remix();
	test_resampler();
	test_bridge_speaker();
	test_bridge_microphone();
	test_bridge_headset_state();
	return test_result("audio");
}
