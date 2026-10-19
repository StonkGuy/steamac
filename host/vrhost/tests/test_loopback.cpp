// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end over loopback: WivrnSession + test pattern (as in `steamac-vrhost --testpattern`) against the fake
// headset: handshake, stream description, tracking control, timesync, all video shards over UDP (IDRs too, as WiVRn), decode, frame
// identity, IDR after a lost frame, and the incompatible-version path; then the same with WiVRn's encryption on:
// PIN pairing (SMP), reconnect of a paired headset, wrong PIN, pairing disabled; and audio both ways plus haptics
// through the VR audio region (a simulated libkrun side on the other end of the shared memory).
#include "audio/audio_bridge.h"
#include "check.h"
#include "fake_headset.h"
#include "pattern_server.h"
#include "pattern/test_pattern.h"
#include "session/clock.h"
#include "wivrn/wivrn_session.h"

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <future>
#include <sys/mman.h>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unistd.h>

#include "crypto.h"
#include "wivrn_packets.h"

using namespace vrhost;

static void print(const char * what, const FakeHeadsetResult & r)
{
	printf("%s: handshake=%s encrypted=%d desc=%d tc=%d timesync=%d decoded=%d/%d mismatch=%d/%d idr=%d/%d "
	       "idr_after_drop=%d decoded_after_drop=%d shards=%d max_shard=%d view_info_ok=%d display_offset=%.1fms error='%s'\n",
	       what, r.handshake_state.c_str(), r.encrypted, r.got_stream_description, r.got_tracking_control,
	       r.timesync_queries, r.decoded[0], r.decoded[1], r.index_mismatch[0], r.index_mismatch[1], r.idr[0], r.idr[1],
	       r.idr_after_drop, r.decoded_after_drop, r.shards, r.max_shard_bytes, r.view_info_ok, r.display_time_offset_ms,
	       r.error.c_str());
}

static double tone_hz(const std::vector<int16_t> & v, uint32_t ch, uint32_t rate, size_t from, size_t to)
{
	int rising = 0;
	size_t first = 0, last = 0;
	for (size_t f = from + 1; f < to; ++f)
		if (v[(f - 1) * ch] < 0 and v[f * ch] >= 0)
		{
			if (not rising)
				first = f;
			last = f;
			rising++;
		}
	return rising > 1 ? double(rising - 1) * rate / double(last - first) : 0;
}

// 7. Audio both ways and haptics, as in a VR session: the region is created like steamac-vm does (shm, unlinked,
// fd passed) and mapped through AudioBridge::map; the libkrun side is simulated by two threads: a "render callback"
// writing a 1 kHz stereo tone into the play ring every 10 ms (a 480-frame CoreAudio IO buffer) with a marker burst,
// and a "capture pump" that marks the mic ring active and reads it every 2 ms. The fake headset plays back what it
// receives and sends a 440 Hz microphone tone with its own marker. Latency bound: marker written -> headset
// received, and headset sent -> mic ring read, each below 30 ms on loopback (the bridge adds at most one 5 ms tick).
static void test_audio()
{
	char name[64];
	snprintf(name, sizeof name, "/vrhost-test-%d", getpid());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	REQUIRE(fd >= 0);
	shm_unlink(name);
	REQUIRE(ftruncate(fd, VRA_REGION_BYTES) == 0);
	{
		void * p = mmap(nullptr, VRA_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		REQUIRE(p != MAP_FAILED);
		vra_region_init(static_cast<vra_region *>(p));
		munmap(p, VRA_REGION_BYTES);
	}
	vra_region * hostside = AudioBridge::map(fd);  // steamac-vrhost --audio-fd
	REQUIRE(hostside != nullptr);
	auto * vm = static_cast<vra_region *>(mmap(nullptr, VRA_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	REQUIRE(vm != MAP_FAILED); // the libkrun side's own mapping
	close(fd);
	{
		int bad = shm_open("/vrhost-test-bad", O_RDWR | O_CREAT, 0600);
		shm_unlink("/vrhost-test-bad");
		ftruncate(bad, VRA_REGION_BYTES);
		CHECK(AudioBridge::map(bad) == nullptr); // zeroed region: refused
		close(bad);
	}

	Server srv({.encryption = WivrnConfig::Encryption::disabled}, hostside);

	std::atomic<bool> stop{false};
	std::atomic<int64_t> marker_written{0}, mic_marker_read{0};
	std::atomic<size_t> written{0}, mic_read{0};
	std::atomic<double> mic_hz{0};
	std::atomic<bool> saw_headset{false};
	std::thread render([&] {
		std::atomic_ref<uint32_t>(vm->play.rate).store(48000, std::memory_order_release);
		const size_t chunk = 480, marker_at = 48000 * 6 / 10; // 600 ms in
		std::vector<int16_t> buf(chunk * 2);
		size_t k = 0;
		auto t = std::chrono::steady_clock::now();
		while (not stop)
		{
			for (size_t f = 0; f < chunk; ++f, ++k)
			{
				int16_t v = int16_t(std::lround(8000 * std::sin(2 * M_PI * 1000 * double(k) / 48000)));
				if (k >= marker_at and k < marker_at + 48)
					v = 30000;
				buf[2 * f] = buf[2 * f + 1] = v;
			}
			if (k - chunk <= marker_at and marker_at < k)
				marker_written = now_ns();
			if (std::atomic_ref<uint32_t>(vm->headset).load() == (VRA_HEADSET_SPEAKER | VRA_HEADSET_MIC))
				saw_headset = true;
			pcm::ring_write(vm->play, vm->play_data, buf.data(), chunk);
			written += chunk;
			t += std::chrono::milliseconds(10);
			std::this_thread::sleep_until(t);
		}
	});
	std::thread capture([&] {
		std::atomic_ref<uint32_t>(vm->mic.active).store(1, std::memory_order_release);
		std::vector<int16_t> got;
		std::vector<int16_t> buf(4096);
		while (not stop)
		{
			size_t n = pcm::ring_read(vm->mic, vm->mic_data, buf.data(), buf.size());
			for (size_t i = 0; i < n; ++i)
				if (buf[i] > 25000 and not mic_marker_read)
					mic_marker_read = now_ns();
			got.insert(got.end(), buf.begin(), buf.begin() + n);
			mic_read += n;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		if (got.size() > 4800)
			mic_hz = tone_hz(got, 1, 48000, 0, std::min<size_t>(got.size(), 48000 * 25 / 100)); // before the marker
	});

	auto fut = std::async(std::launch::async, [&] {
		return run_fake_headset({.port = srv.session.port(), .frames = 110, .speaker_channels = 2, .speaker_rate = 48000,
		                         .mic_channels = 1, .mic_rate = 48000});
	});
	// haptics to the left controller while streaming, one with out-of-range values (clamped by the session)
	for (int i = 0; i < 200 and not srv.session.connected(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	srv.session.haptics(Device::left_grip, 0.1f, 160, 0.5f);
	srv.session.haptics(Device::right_grip, 9.f, NAN, 7.f);
	auto r = fut.get();
	stop = true;
	render.join();
	capture.join();
	print("audio", r);

	CHECK(r.error.empty());
	CHECK(r.got_audio_description);
	CHECK(saw_headset); // the VM side saw the headset's speaker and microphone announced
	CHECK(vm->beat > 20);
	CHECK(not r.mic_feature.empty() and r.mic_feature.back());
	// speaker: everything written while connected arrives in <= 5 ms packets, at 1 kHz, marker within 30 ms
	size_t frames = r.speaker.size() / 2;
	int64_t marker_recv = 0;
	size_t marker_idx = 0;
	for (size_t f = 0; f < frames; ++f)
		if (r.speaker[2 * f] > 25000 and r.speaker[2 * f + 1] > 25000)
		{
			marker_idx = f;
			break;
		}
	for (auto & [t, n]: r.speaker_arrivals)
		if (marker_idx and n > marker_idx)
		{
			marker_recv = t;
			break;
		}
	double spk_lat = marker_recv && marker_written ? (marker_recv - marker_written) / 1e6 : -1;
	double spk_hz = marker_idx > 4800 ? tone_hz(r.speaker, 2, 48000, 0, marker_idx - 10) : 0;
	double mic_lat = mic_marker_read && r.mic_marker_sent_ns ? (mic_marker_read - r.mic_marker_sent_ns) / 1e6 : -1;
	auto & bst = srv.audio->stats();
	printf("audio: speaker %zu frames in %d packets (max %d frames), %.1f Hz, marker latency %.2f ms, discarded %llu; "
	       "mic sent %zu read %zu, %.1f Hz, marker latency %.2f ms; haptics %zu\n",
	       frames, r.speaker_packets, r.max_speaker_packet_frames, spk_hz, spk_lat,
	       (unsigned long long)bst.speaker_discarded.load(), r.mic_frames_sent, mic_read.load(), mic_hz.load(), mic_lat,
	       r.haptics.size());
	CHECK(r.max_speaker_packet_frames <= 240);
	CHECK(std::abs(spk_hz - 1000) < 5);
	CHECK(spk_lat >= 0 and spk_lat < 30);
	CHECK(frames + bst.speaker_discarded >= written.load() * 9 / 10);
	CHECK(r.mic_frames_sent > 4800 and mic_read.load() + 480 >= r.mic_frames_sent);
	CHECK(std::abs(mic_hz.load() - 440) < 5);
	CHECK(mic_lat >= 0 and mic_lat < 30);
	REQUIRE(r.haptics.size() == 2);
	CHECK(r.haptics[0].device == int(wivrn::device_id::LEFT_CONTROLLER_HAPTIC));
	CHECK(std::abs(r.haptics[0].duration_ns - 100'000'000) < 10 and r.haptics[0].frequency == 160 and r.haptics[0].amplitude == 0.5f);
	CHECK(r.haptics[1].device == int(wivrn::device_id::RIGHT_CONTROLLER_HAPTIC));
	CHECK(r.haptics[1].duration_ns == 5'000'000'000 and r.haptics[1].frequency == 0 and r.haptics[1].amplitude == 1.f);
	CHECK(srv.session.stats().haptics == 2);
	munmap(vm, VRA_REGION_BYTES);
	// hostside stays mapped until exit (the bridge is gone with srv)
}

int main()
{
	signal(SIGPIPE, SIG_IGN);
	{
		// Wi-Fi-like link (fake_headset Network): 8 ms + up to 10 ms jitter, 1 % loss, 100 Mbit/s, a 50 ms loss burst
		// every second: every loss is answered with an IDR and decoding carries on.
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		auto r = run_fake_headset({.port = srv.session.port(), .frames = 150,
		                           .net = {.delay_ms = 8, .jitter_ms = 10, .loss_pct = 1, .mbps = 100, .burst_every_s = 1, .burst_ms = 50}});
		print("wifi-like", r);
		CHECK(r.error.empty() and r.decoded[0] >= 150 and r.decoded[1] >= 150);
		CHECK(r.net_dropped > 0 and r.idr[0] > 1); // losses happened and were recovered by IDRs
		srv.wait_disconnect();
	}
	{
		// 1. encryption disabled, a lost frame on stream 0
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		int port = srv.session.port();
		REQUIRE(port > 0);
		auto r = run_fake_headset({.port = port, .frames = 60, .drop_frame = 20});
		print("plain", r);
		CHECK(r.error.empty());
		CHECK(r.handshake_state == "encryption_disabled");
		CHECK(not r.encrypted);
		CHECK(r.got_stream_description);
		CHECK(r.got_tracking_control);
		CHECK(r.control_shards == 0); // an IDR on TCP can arrive after the next P frames and be dropped as old
		CHECK(r.timesync_queries > 0);
		CHECK(r.decoded[0] >= 60 and r.decoded[1] >= 60);
		CHECK(r.index_mismatch[0] == 0 and r.index_mismatch[1] == 0);
		CHECK(r.idr_after_drop >= 1);    // the lost frame triggered a new IDR
		CHECK(r.decoded_after_drop > 0); // and decoding resumed
		CHECK(r.view_info_ok);
		// display time is announced in the headset clock (offset 5 s), predicted ahead of "now"
		CHECK(r.display_time_offset_ms > -20 and r.display_time_offset_ms < 100);
		CHECK(srv.session.stats().idr_requests >= 1);
		CHECK(srv.session.stats().tracking > 0);
		srv.wait_disconnect();

		// 2. wrong protocol version is refused like WiVRn does
		auto bad = run_fake_headset({.port = port, .protocol_version = 0x1234, .timeout_ms = 3000});
		CHECK(bad.handshake_state == "incompatible_version");
	}

	{
		// 1c. a producer that misses frames (asks for a frame timing, submits nothing): the frame indices on the
		// wire stay consecutive, so the headset reports no lost frame and no IDR is requested (the real WiVRn client
		// answered every skipped index with an IDR request: 990 requests in 75 s of a 25 fps game on a 60 Hz headset)
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		srv.skip_every = 3;
		auto r = run_fake_headset({.port = srv.session.port(), .frames = 90, .decode_ns = 8'000'000,
		                           .display_period_ns = 13'888'889, .display_phase_ns = 3'300'000});
		print("skipping", r);
		CHECK(r.error.empty());
		CHECK(r.decoded[0] >= 90 and r.decoded[1] >= 90);
		CHECK(r.index_gaps[0] == 0 and r.index_gaps[1] == 0);
		CHECK(srv.session.stats().idr_requests == 0);
		CHECK(r.idr[0] == 1 and r.idr[1] == 1);
	}

	{
		// 1b. pacing from the headset's feedback: the headset decodes in 8 ms and blits on its own 72 Hz vsync grid
		// (phase 3.3 ms); WiVRn's pacer must phase-lock the announced display times to that grid.
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		const int64_t period = 13'888'889, phase = 3'300'000;
		auto r = run_fake_headset({.port = srv.session.port(), .frames = 250, .decode_ns = 8'000'000,
		                           .display_period_ns = period, .display_phase_ns = phase});
		print("paced", r);
		CHECK(r.error.empty());
		int on_grid = 0, n = 0;
		for (size_t i = r.display_times.size() > 40 ? r.display_times.size() - 40 : 0; i < r.display_times.size(); ++i, ++n)
		{
			int64_t off = ((r.display_times[i].second - phase) % period + period) % period;
			off = std::min(off, period - off);
			on_grid += off < 1'000'000;
		}
		printf("paced: %d/%d of the last display times within 1 ms of the headset vsync grid\n", on_grid, n);
		CHECK(n >= 30 and on_grid >= n * 9 / 10);
	}

	char dir_template[] = "/tmp/vrhost-keys.XXXXXX";
	REQUIRE(mkdtemp(dir_template) != nullptr);
	std::string keys_file = std::string(dir_template) + "/known_keys";
	std::string headset_key = crypto::key::generate_x448_keypair().private_key();
	{
		// 3. pairing: the headset proves the PIN (SMP), then everything is AES-128-CTR, UDP shards included
		Server srv({.encryption = WivrnConfig::Encryption::pairing, .pin = "481516", .known_keys_file = keys_file});
		int port = srv.session.port();
		CHECK(srv.session.pin() == "481516");
		auto r = run_fake_headset({.port = port, .frames = 40, .drop_frame = 10, .private_key = headset_key, .pin = "481516"});
		print("pairing", r);
		CHECK(r.error.empty());
		CHECK(r.handshake_state == "pin_needed");
		CHECK(r.encrypted);
		CHECK(r.got_stream_description and r.got_tracking_control and r.timesync_queries > 0);
		CHECK(r.decoded[0] >= 40 and r.decoded[1] >= 40);
		CHECK(r.index_mismatch[0] == 0 and r.index_mismatch[1] == 0);
		CHECK(r.idr_after_drop >= 1 and r.decoded_after_drop > 0);
		CHECK(r.view_info_ok);
		CHECK(srv.session.stats().paired == 1);
		srv.wait_disconnect();

		// 4. the paired headset reconnects without a PIN
		auto again = run_fake_headset({.port = port, .frames = 20, .private_key = headset_key, .pin = "000000"});
		print("paired", again);
		CHECK(again.error.empty());
		CHECK(again.handshake_state == "client_already_paired");
		CHECK(again.encrypted and again.decoded[0] >= 20 and again.decoded[1] >= 20);
		srv.wait_disconnect();

		// 5. a new headset with the wrong PIN is refused and not stored
		auto wrong = run_fake_headset({.port = port, .frames = 5, .timeout_ms = 5000, .pin = "111111"});
		print("wrong pin", wrong);
		CHECK(wrong.handshake_state == "pin_needed");
		CHECK(wrong.error == "Incorrect PIN");
		CHECK(wrong.decoded[0] == 0);
		CHECK(srv.session.stats().paired == 1);
		srv.wait_disconnect();

		// 5b. PIN guessing is throttled: after a second wrong PIN the next attempt is refused for a while, even with
		// the right PIN (WiVRn itself has no limit; 6 digits fall to an online search without one)
		auto wrong2 = run_fake_headset({.port = port, .frames = 5, .timeout_ms = 5000, .pin = "222222"});
		CHECK(wrong2.error == "Incorrect PIN");
		srv.wait_disconnect();
		auto throttled = run_fake_headset({.port = port, .frames = 5, .timeout_ms = 5000, .pin = "481516"});
		print("throttled", throttled);
		CHECK(not throttled.error.empty() and throttled.decoded[0] == 0);
		CHECK(srv.session.stats().paired == 1);
		srv.wait_disconnect();
	}
	{
		// 6. encryption on, pairing off: the paired headset connects, an unknown one gets pairing_disabled
		Server srv({.encryption = WivrnConfig::Encryption::enabled, .known_keys_file = keys_file});
		int port = srv.session.port();
		CHECK(srv.session.pin().empty());
		auto known = run_fake_headset({.port = port, .frames = 10, .private_key = headset_key});
		print("enabled/known", known);
		CHECK(known.error.empty() and known.handshake_state == "client_already_paired" and known.decoded[0] >= 10);
		srv.wait_disconnect();
		auto unknown = run_fake_headset({.port = port, .frames = 5, .timeout_ms = 3000});
		CHECK(unknown.handshake_state == "pairing_disabled");
	}
	{
		// 7. the headset leaves while the host's audio, haptics and microphone paths are sending: the failed send must
		// not escape on those threads (the receive loop handles the disconnect, as for video shards)
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		std::atomic<bool> done{false};
		std::atomic<int> sends_after_close{0};
		std::atomic<bool> closed{false};
		// back to back from two threads: the first send after the headset's reset fails, before the receive loop has
		// dropped the connection
		auto send_loop = [&] {
			std::vector<int16_t> pcm(96, 1000);
			for (bool mic = false; not done; mic = not mic)
			{
				srv.session.send_audio(pcm);
				srv.session.haptics(Device::left_grip, 0.01f, 160, 0.5f);
				srv.session.set_microphone(mic);
				if (closed)
					sends_after_close++;
			}
		};
		std::thread sender(send_loop), sender2(send_loop);
		auto r = run_fake_headset({.port = srv.session.port(), .frames = 10});
		closed = true; // the fake headset's sockets are closed when it returns
		CHECK(r.error.empty() and r.decoded[0] >= 10);
		srv.wait_disconnect();
		// keep sending into the dead connection's teardown window and beyond
		for (int i = 0; i < 200 and sends_after_close < 50; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		done = true;
		sender.join();
		sender2.join();
		CHECK(sends_after_close >= 50 and not srv.session.connected());
	}
	{
		// 8. an IPv4 headset (the dual-stack listener sees a v4-mapped peer): its UDP stream handshake must be taken as
		// from the control peer (macOS reports the IPv4 sender of the AF_INET6 stream socket as a sockaddr_in), with
		// encryption off and with PIN pairing after the user took a while to type the PIN
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		auto r = run_fake_headset({.port = srv.session.port(), .host = "127.0.0.1", .frames = 10, .timeout_ms = 15000});
		print("ipv4", r);
		CHECK(r.error.empty() and r.handshake_state == "encryption_disabled" and r.control_shards == 0);
		CHECK(r.decoded[0] >= 10 and r.decoded[1] >= 10);
		srv.wait_disconnect();
	}
	{
		Server srv({.encryption = WivrnConfig::Encryption::pairing, .pin = "271828", .known_keys_file = keys_file});
		auto r = run_fake_headset({.port = srv.session.port(), .host = "127.0.0.1", .pin_delay_ms = 1500, .frames = 10,
		                           .timeout_ms = 15000, .pin = "271828"});
		print("ipv4 pairing", r);
		CHECK(r.error.empty() and r.handshake_state == "pin_needed" and r.encrypted and r.control_shards == 0);
		CHECK(r.decoded[0] >= 10 and r.decoded[1] >= 10);
		CHECK(srv.session.stats().paired == 1);
		srv.wait_disconnect();
	}
	std::filesystem::remove_all(dir_template);
	test_audio();
	{
		// 9. headset_info from the network is checked: a 0x0 stream is refused (the session keeps serving), a NaN
		// refresh rate falls back to an offered one instead of reaching the pacer (1e9 / NaN -> integer: undefined)
		Server srv({.encryption = WivrnConfig::Encryption::disabled});
		auto zero = run_fake_headset({.port = srv.session.port(), .frames = 5, .eye_width = 0, .eye_height = 0, .timeout_ms = 3000});
		CHECK(not zero.error.empty() and not zero.got_stream_description);
		auto nan = run_fake_headset({.port = srv.session.port(), .frames = 10, .refresh_hz = std::nanf("")});
		print("nan refresh", nan);
		CHECK(nan.error.empty() and nan.decoded[0] >= 10);
		CHECK(srv.session.refresh_rate() == 0 or std::isfinite(srv.session.refresh_rate()));
		srv.wait_disconnect();
		// an audio format the bridge cannot serve (50 Hz: 0-frame packets) is not taken: no audio stream at all
		auto slow = run_fake_headset({.port = srv.session.port(), .frames = 5, .speaker_channels = 2, .speaker_rate = 50});
		CHECK(slow.error.empty() and not slow.got_audio_description);
	}
	return test_result("loopback");
}
