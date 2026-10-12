// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter on loopback against the test headset (fake_vd_client, written from the notes): discovery
// with account approval (pairing window, refused stranger), session key + computer record, the four channels with the
// AES wrapper, settings sync both ways (mobile in -> HeadsetInfo, refresh and render-divisor changes; streamer out),
// VR video format, VideoToolbox HEVC frames per eye with FrameMetadata (pose, FOV, timing, IDR), ForceKeyFrame,
// tracking + input up, haptics folded into the frame metadata, speaker audio down, microphone up, and the control
// socket status through VdControl. Discovery key: random per run (never a real key).
#include "check.h"
#include "control/control_server.h"
#include "fake_vd_client.h"
#include "pattern_server.h"
#include "vd/vd_session.h"
#include "wivrn/input_map.h"

#include <cmath>
#include <csignal>
#include <filesystem>
#include <mutex>

using namespace vrhost;
using namespace vrhost::vd;

struct Listener : SessionListener
{
	std::mutex m;
	int connected = 0, disconnected = 0;
	HeadsetInfo info;
	uint64_t tracking = 0;
	std::optional<TrackedPose> head, right;
	std::vector<InputValue> inputs;
	std::vector<int16_t> mic;
	std::vector<float> refresh;
	std::vector<uint32_t> divisors;
	void on_connected(const HeadsetInfo & h) override
	{
		std::lock_guard l(m);
		connected++;
		info = h;
	}
	void on_disconnected() override
	{
		std::lock_guard l(m);
		disconnected++;
	}
	void on_tracking(std::span<const TrackedPose> p) override
	{
		std::lock_guard l(m);
		tracking += p.size();
		for (auto & t: p)
			if (t.device == Device::head)
				head = t;
			else if (t.device == Device::right_grip)
				right = t;
	}
	void on_input(std::span<const InputValue> v) override
	{
		std::lock_guard l(m);
		inputs.insert(inputs.end(), v.begin(), v.end());
	}
	void on_microphone(std::span<const int16_t> pcm, ns) override
	{
		std::lock_guard l(m);
		mic.insert(mic.end(), pcm.begin(), pcm.end());
	}
	void on_refresh_rate(float hz) override
	{
		std::lock_guard l(m);
		refresh.push_back(hz);
	}
	void on_render_rate(uint32_t d) override
	{
		std::lock_guard l(m);
		divisors.push_back(d);
	}
};

static int16_t tone(size_t i)
{
	return int16_t(8000 * std::sin(2 * M_PI * 440.0 * double(i) / 48000));
}

int main()
{
	signal(SIGPIPE, SIG_IGN);
	char dir_template[] = "/tmp/vrhost-vd.XXXXXX";
	REQUIRE(mkdtemp(dir_template) != nullptr);
	std::string dir = dir_template;
	AesKey disc = AesKey::random();

	Listener listener;
	VdSession session({
	        .control_port = 0,
	        .data_port = 0,
	        .video_port = 0,
	        .audio_port = 0,
	        .discovery_port = 0,
	        .discovery_key = disc,
	        .accounts_file = dir + "/vd_accounts",
	        .addresses = {"127.0.0.1"},
	        .bitrate_bps = 300'000'000, // above the cap: clamped to 200
	        .log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); },
	});
	session.set_microphone(true);
	session.start(&listener);
	auto ports = session.ports();
	CHECK(ports.control and ports.data and ports.video and ports.audio and ports.discovery);
	CHECK(session.pairing()); // nothing allowed yet: the first account may pair

	// --- discovery: the pairing window admits one account, a stranger is refused, the owner comes back ---------
	FakeVdOptions o{.discovery_port = ports.discovery, .discovery_key = disc, .account = "owner"};
	auto first = fake_vd_discover(o);
	REQUIRE(first);
	CHECK(not session.pairing());
	CHECK(first->record.os == "Windows" and first->record.version == "1.34.22" and first->record.encrypt_local);
	CHECK(first->record.control_port == ports.control and first->record.data_port == ports.data and
	      first->record.video_port == ports.video and first->record.audio_port == ports.audio);
	CHECK(first->record.addresses == std::vector<std::string>{"127.0.0.1"});
	FakeVdOptions stranger = o;
	stranger.account = "stranger";
	CHECK(not fake_vd_discover(stranger, 500));
	CHECK(session.stats().offers_refused == 1);
	FakeVdOptions wrong_key = o;
	wrong_key.discovery_key = AesKey::random();
	CHECK(not fake_vd_discover(wrong_key, 300));

	// --- control socket while waiting ------------------------------------------------------------------------
	VdControl control(session);
	ControlServer ctl(dir + "/control.sock", control);
	auto st = ctl.handle("status");
	printf("status: %s\n", st.c_str());
	CHECK(st.find("\"state\":\"waiting\"") != std::string::npos and st.find("\"adapter\":\"vd\"") != std::string::npos);
	CHECK(st.find("\"port\":" + std::to_string(ports.control)) != std::string::npos);
	CHECK(ctl.handle("headsets").find("\"name\":\"owner\"") != std::string::npos);

	// --- producer: what steamac-vrhost --testpattern does, plus speaker audio and a haptic pulse --------------
	std::atomic<bool> stop{false};
	std::atomic<int> submitted{0};
	size_t tone_pos = 0;
	std::thread producer([&] {
		TestPattern pattern;
		std::array<CVPixelBufferRef, 2> bufs{};
		uint32_t bw = 0;
		while (not stop)
		{
			auto info = session.headset();
			if (not session.connected() or not info)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			if (bw != info->eye_width)
			{
				for (auto & b: bufs)
				{
					if (b)
						CVPixelBufferRelease(b);
					b = make_nv12(info->eye_width, info->eye_height);
				}
				bw = info->eye_width;
			}
			FrameTiming t = session.next_frame_timing();
			Frame f;
			f.frame_id = t.frame_id;
			f.display_time = t.predicted_display;
			session.views_at(t.predicted_display, f.pose, f.fov);
			for (int i = 0; i < 2; ++i)
			{
				pattern.render(bufs[i], f.pose[i], f.fov[i], f.frame_id);
				f.eye[i].surface = CVPixelBufferGetIOSurface(bufs[i]);
			}
			session.submit(f);
			std::vector<int16_t> pcm(480 * 2);
			for (size_t i = 0; i < 480; ++i, ++tone_pos)
				pcm[2 * i] = pcm[2 * i + 1] = tone(tone_pos);
			session.send_audio(pcm);
			if (++submitted == 4)
			{
				session.haptics(Device::right_grip, 0.5f, 160, 0.7f);
				session.set_streamer_setting("AllowMotionExtrapolation", "false");
			}
		}
		for (auto b: bufs)
			if (b)
				CVPixelBufferRelease(b);
	});

	// --- the session ----------------------------------------------------------------------------------------
	FrameData fd;
	fd.head.orientation_tracked = fd.head.position_tracked = 1;
	fd.head.position = {0, 1.6f, 0};
	fd.controller[1].orientation_tracked = fd.controller[1].position_tracked = 1;
	fd.controller[1].position = {0.2f, 1.2f, -0.3f};
	fd.input.hand[1].trigger = 204;
	fd.input.hand[1].buttons = button_a;
	fd.input.hand[0].thumb_y = -32767;
	o.frame_data = fd;
	o.frames = 16;
	o.force_key_frame_after = 6;
	for (size_t i = 0; i < 480; ++i)
		o.microphone.push_back(tone(i));
	o.mobile_changes = {{"UseSpacewarp", "true"}, {"Framerate", "72"}};
	auto r = run_fake_vd_client(o);
	if (not r.error.empty())
		fprintf(stderr, "fake client: %s\n", r.error.c_str());
	CHECK(r.error.empty());
	CHECK(r.peer_encrypt);
	CHECK(r.streamer_settings.find("\"AllowMotionExtrapolation\":true") != std::string::npos and
	      r.streamer_settings.find("\"VRAvailable\":true") != std::string::npos);
	CHECK((r.streamer_changes == std::vector<std::pair<std::string, std::string>>{{"AllowMotionExtrapolation", "false"}}));

	// video format: VR, HEVC, Quest 3 Low floor, two per-eye streams; re-sent when the headset changed its rate
	REQUIRE(not r.formats.empty());
	auto & f0 = r.formats.front();
	CHECK(f0.source == Source::vr and f0.codec == VideoCodec::hevc and f0.width == 1700 and f0.height == 1700 and
	      f0.stream_count == 2 and f0.frame_rate == 90 and f0.stream[1].bounds.x == 1700);
	CHECK(r.formats.size() == 2 and r.formats.back().frame_rate == 72);

	// frames: both eyes decode, length-prefixed NALs, IDR first and again after ForceKeyFrame / the format change
	printf("frames %zu slices %zu decoded %d/%d idr %d/%d\n", r.frames.size(), r.slices.size(), r.decoded[0], r.decoded[1],
	       r.idr[0], r.idr[1]);
	CHECK(r.frames.size() >= 16 and r.slices.size() == r.frames.size());
	CHECK(r.decoded[0] >= 14 and r.decoded[1] >= 14);
	CHECK(r.length_prefix_errors == 0); // TODO(vd-live:annexb-default): length-prefixed unless UseAnnexB
	CHECK(r.idr[0] >= 3 and r.idr[0] == r.idr[1]);
	CHECK(session.stats().idr_requests == 1);
	REQUIRE(not r.frames.empty());
	CHECK(r.frames[0].is_sync_frame == 1);
	bool haptic_seen = false, fields_ok = true;
	for (size_t i = 0; i < r.frames.size(); ++i)
	{
		auto & md = r.frames[i];
		fields_ok = fields_ok and md.frame_index == int64_t(r.frames[0].frame_index + i) and md.source == Source::vr and
		            md.render_width == 1700 and md.pose.has_valid_pose and md.pose.fov[0].left > 0 and md.pose.fov[0].up > 0;
		fields_ok = fields_ok and r.slices[i].timestamp_us == md.timestamp_us and r.slices[i].stream_index == 1;
		if (i)
			fields_ok = fields_ok and md.timestamp_us > r.frames[i - 1].timestamp_us;
		if (md.haptic[1].amplitude > 0.69f and md.haptic[1].amplitude < 0.71f and md.haptic[0].amplitude == 0)
			haptic_seen = true;
	}
	CHECK(fields_ok);
	CHECK(haptic_seen);
	// frame time: 90 Hz at first, then 72 Hz with the render divisor 2 (SSW on): 27.8 ms per rendered frame
	CHECK(std::abs(r.frames.front().frame_time_ms - 11.11f) < 0.1f);
	CHECK(std::abs(r.frames.back().frame_time_ms - 27.78f) < 0.1f);
	CHECK(r.frames.back().encode_time_ms > 0 and r.frames.back().encode_time_ms < 100);

	// head/controller pose and inputs reached the listener, in the guest's input table
	std::array<Pose, 2> eyes;
	std::array<Fov, 2> fov;
	session.views_at(now_ns(), eyes, fov);
	CHECK(std::abs(eyes[1].position[0] - eyes[0].position[0] - 0.064f) < 1e-4f and fov[0].left < 0 and fov[0].up > 0);
	{
		std::lock_guard l(listener.m);
		CHECK(listener.head and std::abs(listener.head->pose.position[1] - 1.6f) < 1e-4f and listener.head->position_valid);
		CHECK(listener.right and std::abs(listener.right->pose.position[2] + 0.3f) < 1e-4f);
		CHECK(listener.connected == 1 and listener.info.name == "Quest3" and listener.info.eye_width == 1700);
		CHECK(listener.info.foveation == FoveationModel::none and listener.info.render_eye_width == 1700);
		CHECK(listener.info.max_bitrate_bps == 200'000'000 and listener.info.bitrate_bps == 150'000'000);
		CHECK(listener.info.speaker and listener.info.speaker->channels == 2 and listener.info.microphone->rate == 48000);
		CHECK(listener.tracking >= 5 * 10);
		bool trigger = false, a = false, stick = false;
		for (auto & v: listener.inputs)
		{
			int hand, index;
			if (not wivrn_input_to_vrl(v.id, hand, index))
				continue;
			trigger |= hand == 1 and index == svr::trigger_value and std::abs(v.value - 0.8f) < 1e-3f;
			a |= hand == 1 and index == svr::a_click and v.value == 1;
			stick |= hand == 0 and index == svr::thumbstick_y and v.value == -1;
		}
		CHECK(trigger and a and stick);
		CHECK(listener.inputs.size() == 3); // on change only
		CHECK((listener.refresh == std::vector<float>{72}));
		CHECK((listener.divisors == std::vector<uint32_t>{2}));
		CHECK(listener.mic.size() == 480 and listener.mic[100] == tone(100));
	}
	CHECK(session.render_divisor() == 2);

	// a second session of the same account (reconnect), asking for Annex-B and H.264, without the audio channel
	{
		FakeVdOptions o2{.discovery_port = ports.discovery, .discovery_key = disc, .account = "owner"};
		o2.mobile_settings = R"({"HmdType":"Quest3","Framerate":80,"UseAnnexB":true,"VideoCodec":"H264"})";
		o2.frames = 3;
		o2.annexb = true;
		o2.audio = false;
		auto r2 = run_fake_vd_client(o2);
		CHECK(r2.error.empty() and r2.length_prefix_errors == 0 and r2.decoded[0] >= 2 and r2.decoded[1] >= 2);
		CHECK(not r2.formats.empty() and r2.formats[0].codec == VideoCodec::h264 and r2.formats[0].frame_rate == 80);
		for (int i = 0; i < 100 and session.connected(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::lock_guard l(listener.m);
		CHECK(listener.connected == 2 and listener.disconnected == 2 and listener.info.preferred_refresh_rate == 80);
	}

	// speaker audio: a contiguous run of the tone (packets before the audio channel was up are dropped)
	CHECK(r.speaker_packets > 0 and r.speaker.size() >= 960);
	bool speaker_ok = false;
	for (size_t k = 0; k < 48000 and not speaker_ok and r.speaker.size() >= 960; ++k)
	{
		speaker_ok = true;
		for (size_t i = 0; i < 480 and speaker_ok; ++i)
			speaker_ok = r.speaker[2 * i] == tone(k + i) and r.speaker[2 * i + 1] == tone(k + i);
	}
	CHECK(speaker_ok);

	// the client said Disconnect and closed: the session ends
	for (int i = 0; i < 100 and session.connected(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(not session.connected());
	st = ctl.handle("status");
	CHECK(st.find("\"frames\":" + std::to_string(session.stats().frames_sent.load())) != std::string::npos);
	CHECK(ctl.handle("pair on").find("\"state\":\"pairing\"") != std::string::npos and session.pairing());
	CHECK(ctl.handle("unpair owner").find("owner") == std::string::npos);

	stop = true;
	producer.join();
	session.stop();
	std::filesystem::remove_all(dir);
	return test_result("vd_loopback");
}
