// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: the headset's Games tab against the test headset (fake_vd_client). The headset asks for
// the games list (RequestGames) and gets the offered Steam apps (Games: platform, app id, running); it picks one
// (LaunchGame) and the pick shows once in the control socket's app_request, which the launcher turns into exactly
// one guest `launch-vr <id>` (the guest agent then skips a game that already runs). Picks of games that were not
// offered, or of another platform, change nothing. Discovery key: random per run (never a real key).
#include "check.h"
#include "control/control_server.h"
#include "fake_vd_client.h"
#include "pattern_server.h"
#include "vd/vd_session.h"

#include <algorithm>
#include <csignal>
#include <filesystem>

using namespace vrhost;
using namespace vrhost::vd;

int main()
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	char dir_template[] = "/tmp/vrhost-vdg.XXXXXX";
	REQUIRE(mkdtemp(dir_template) != nullptr);
	std::string dir = dir_template;
	AesKey disc = AesKey::random();

	VdSession session({
	        .control_port = 0,
	        .data_port = 0,
	        .video_port = 0,
	        .audio_port = 0,
	        .discovery_port = 0,
	        .discovery_key = disc,
	        .accounts_file = dir + "/vd_accounts",
	        .pairing = true,
	        .addresses = {"127.0.0.1"},
	        .applications = {{"438100", "VRChat"}, {"620980", "Beat Saber"}, {"not-a-steam-id", "Other"}},
	        .log = [](const std::string & s) { printf("[server] %s\n", s.c_str()); },
	});
	SessionListener listener;
	session.start(&listener);
	auto ports = session.ports();
	VdControl control(session);
	ControlServer ctl(dir + "/control.sock", control);
	CHECK(ctl.handle("status").find("\"app_request\":{\"id\":\"\",\"seq\":0}") != std::string::npos);

	std::atomic<bool> stop{false};
	std::thread producer([&] {
		TestPattern pattern;
		std::array<CVPixelBufferRef, 2> bufs{};
		while (not stop)
		{
			auto info = session.headset();
			if (not session.connected() or not info)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			if (not bufs[0])
				for (auto & b: bufs)
					b = make_nv12(info->eye_width, info->eye_height);
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
		}
		for (auto b: bufs)
			if (b)
				CVPixelBufferRelease(b);
	});

	auto id = [](GamePlatform p, uint64_t v) {
		auto b = game_id_payload(p, v);
		return std::string(b.begin(), b.end());
	};
	FakeVdOptions o{.discovery_port = ports.discovery,
	                .control_port = ports.control,
	                .data_port = ports.data,
	                .video_port = ports.video,
	                .audio_port = ports.audio,
	                .discovery_key = disc,
	                .account = "owner"};
	o.audio = false;
	o.decode = false;
	o.frames = 14;
	o.control_schedule = {
	        {2, uint8_t(Msg::request_games), std::string()},
	        {4, uint8_t(Msg::request_games), std::string(1, '\1')}, // rescan flag
	        {6, uint8_t(Msg::launch_game), id(GamePlatform::steam, 438100)},
	        {8, uint8_t(Msg::launch_game), id(GamePlatform::steam, 999999)},       // not offered
	        {8, uint8_t(Msg::launch_game), id(GamePlatform::oculus_rift, 620980)}, // other platform
	        {8, uint8_t(Msg::launch_game), std::string("\1\0", 2)},                // truncated
	};
	auto r = run_fake_vd_client(o);
	if (not r.error.empty())
		fprintf(stderr, "client: %s\n", r.error.c_str());
	CHECK(r.error.empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the last control messages reach the host

	// --- the games list: once per request, the two Steam apps, in order, not running -------------------------
	int lists = 0;
	for (auto & [type, payload]: r.control_messages)
	{
		if (Msg(type) != Msg::games)
			continue;
		lists++;
		auto g = parse_games(payload);
		CHECK(g.has_value());
		if (not g)
			continue;
		printf("games list: %zu entries\n", g->size());
		CHECK(g->size() == 2);
		if (g->size() == 2)
		{
			CHECK((*g)[0].platform == GamePlatform::steam and (*g)[0].id == 438100 and not(*g)[0].running);
			CHECK((*g)[1].platform == GamePlatform::steam and (*g)[1].id == 620980 and not(*g)[1].running);
		}
	}
	CHECK(lists == 2);
	// wire layout pinned: count 2, then 01 | 00..00 06 AF 54 | 00 for VRChat
	std::vector<GameEntry> one{{GamePlatform::steam, 438100, false}};
	CHECK((games_payload(one) == std::vector<uint8_t>{0, 1, 1, 0, 0, 0, 0, 0, 0x06, 0xAF, 0x54, 0}));

	// --- the pick: one launch request for VRChat, nothing for the rest ----------------------------------------
	auto a = session.app_request();
	printf("app_request: id=%s seq=%llu\n", a.id.c_str(), (unsigned long long)a.seq);
	CHECK(a.id == "438100" and a.seq == 1);
	auto st = ctl.handle("status");
	CHECK(st.find("\"app_request\":{\"id\":\"438100\",\"seq\":1}") != std::string::npos);
	CHECK(session.stats().bad_messages >= 1); // the truncated LaunchGame

	// the launcher's relay (VRHost.swift VRRelay): a launch-vr per seq step, so polling twice starts it once
	std::vector<std::string> sent;
	uint64_t last = 0;
	for (int poll = 0; poll < 3; ++poll)
	{
		auto now = control.app_request();
		if (now.seq != last and now.seq > 0)
			sent.push_back("launch-vr " + now.id);
		last = now.seq;
	}
	CHECK(sent == std::vector<std::string>{"launch-vr 438100"});

	stop = true;
	producer.join();
	session.stop();
	std::filesystem::remove_all(dir);
	return test_result("vd_games");
}
