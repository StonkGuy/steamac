// SPDX-License-Identifier: GPL-3.0-or-later
// Control socket (docs/vr-interfaces.md section 5) over a real AF_UNIX socket against a loopback session: status
// while waiting, pairing switched on at run time (fresh PIN) and a new headset pairing with exactly that PIN, the
// connected headset's info and stream counters in the status, the paired list, unpair, pairing off, bad requests.
#include "check.h"
#include "control/control_server.h"
#include "fake_headset.h"
#include "pattern_server.h"

#include <csignal>
#include <filesystem>
#include <mutex>
#include <vector>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "crypto.h"

static std::string ask(const std::string & path, const std::string & req)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path.c_str());
	if (connect(fd, (sockaddr *)&a, sizeof a) != 0)
	{
		close(fd);
		return "";
	}
	std::string line = req + "\n", out;
	send(fd, line.data(), line.size(), 0);
	char buf[512];
	ssize_t n;
	while ((n = recv(fd, buf, sizeof buf, 0)) > 0)
		out.append(buf, n);
	close(fd);
	printf("> %s\n< %s", req.c_str(), out.c_str());
	return out;
}

static bool has(const std::string & s, const std::string & what)
{
	return s.find(what) != std::string::npos;
}

static std::string field(const std::string & s, const std::string & key) // string value of "key":"..."
{
	auto k = s.find("\"" + key + "\":\"");
	if (k == std::string::npos)
		return "";
	k += key.size() + 4;
	return s.substr(k, s.find('"', k) - k);
}

int main()
{
	signal(SIGPIPE, SIG_IGN);
	char dir_template[] = "/tmp/vrhost-ctl.XXXXXX";
	REQUIRE(mkdtemp(dir_template) != nullptr);
	std::string dir = dir_template, keys_file = dir + "/known_keys", sock = dir + "/control.sock";
	std::string headset_key = crypto::key::generate_x448_keypair().private_key();
	{
		// encryption on, nothing paired, pairing off: what a host started without --pair after unpairing everything
		Server srv({.encryption = WivrnConfig::Encryption::enabled, .known_keys_file = keys_file});
		ControlServer ctl(sock, srv.session);
		CHECK(ControlServer::json_string("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");

		auto s = ask(sock, "status");
		CHECK(has(s, "\"v\":1") and field(s, "state") == "waiting" and field(s, "encryption") == "enabled");
		CHECK(field(s, "pin").empty() and has(s, "\"headset\":null"));
		CHECK(has(s, "\"port\":" + std::to_string(srv.session.port())));

		// pairing on at run time: fresh 6-digit PIN, a new headset pairs with it
		s = ask(sock, "pair on");
		std::string pin = field(s, "pin");
		CHECK(field(s, "state") == "pairing" and pin.size() == 6);
		CHECK(srv.session.pin() == pin);
		auto wrong = run_fake_headset({.port = srv.session.port(), .frames = 5, .timeout_ms = 5000, .pin = pin == "111111" ? "222222" : "111111"});
		CHECK(wrong.error == "Incorrect PIN");
		srv.wait_disconnect();

		std::atomic<bool> saw_connected{false};
		std::string connected_status;
		std::thread poller([&] {
			for (int i = 0; i < 400 and not saw_connected; ++i)
			{
				auto st = ask(sock, "status");
				if (field(st, "state") == "connected" and not has(st, "\"frames\":0,"))
				{
					connected_status = st;
					saw_connected = true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
		});
		auto r = run_fake_headset({.port = srv.session.port(), .frames = 60, .private_key = headset_key, .pin = pin});
		poller.join();
		CHECK(r.error.empty() and r.handshake_state == "pin_needed" and r.decoded[0] >= 60);
		CHECK(saw_connected);
		CHECK(has(connected_status, "\"eye_width\":640") and has(connected_status, "\"eye_height\":704"));
		CHECK(has(connected_status, "\"headset\":{\"name\":"));
		CHECK(has(connected_status, "\"target_mbps\":50.0"));
		srv.wait_disconnect();

		auto h = ask(sock, "headsets");
		CHECK(has(h, "\"headsets\":[{\"name\":") and has(h, "\"last_connection\":"));

		// pairing off: no PIN, state back to waiting, the paired headset still connects
		s = ask(sock, "pair off");
		CHECK(field(s, "state") == "waiting" and field(s, "pin").empty() and field(s, "encryption") == "enabled");
		auto again = run_fake_headset({.port = srv.session.port(), .frames = 10, .private_key = headset_key});
		CHECK(again.error.empty() and again.handshake_state == "client_already_paired");
		srv.wait_disconnect();

		// unpair it: the list is empty and it is refused (pairing off)
		h = ask(sock, "unpair-all");
		CHECK(has(h, "\"headsets\":[]"));
		auto refused = run_fake_headset({.port = srv.session.port(), .frames = 5, .timeout_ms = 3000, .private_key = headset_key});
		CHECK(refused.handshake_state == "pairing_disabled");
		s = ask(sock, "status");
		CHECK(has(s, "\"pairing_request\":{\"name\":\"steamac fake headset\",\"at\":") and has(s, "\"refused\":true}"));
		CHECK(has(s, "\"app_request\":{\"id\":\"\",\"seq\":0}"));
		CHECK(has(ask(sock, "refresh 90"), "\"error\"")); // no headset connected
		CHECK(has(ask(sock, "bitrate 0"), "\"error\"") and has(ask(sock, "bitrate 12x"), "\"error\""));
		CHECK(has(ask(sock, "bitrate 80"), "\"target_mbps\":80.0"));
		CHECK(srv.session.bitrate_bps() == 80'000'000u);

		CHECK(has(ask(sock, "bogus"), "\"error\":\"unknown request\""));
		CHECK(has(ask(sock, "unpair "), "\"error\""));
	}
	CHECK(not std::filesystem::exists(sock)); // removed with the server
	{
		// pairing off with --pair-on-request: an unpaired headset opens pairing (the PIN goes to on_pin, as the PIN
		// file and the launcher's panel), pairs with it, and pairing closes again. Also: H.264 chosen by the host,
		// battery, the app list and a start request, refresh rate and bitrate changes while connected.
		std::string keys2 = dir + "/known_keys2";
		std::mutex m;
		std::vector<std::string> pins;
		Server srv({.encryption = WivrnConfig::Encryption::enabled,
		            .pin = "424242",
		            .known_keys_file = keys2,
		            .pair_on_request = true,
		            .render_scale = 0.5f,
		            .keyframe_interval_s = 0.25f,
		            .prioritize_speed = true,
		            .codec = WivrnConfig::Codec::h264,
		            .applications = {{"438100", "VRChat"}},
		            .on_pin = [&](const std::string & p) {
			            std::lock_guard l(m);
			            pins.push_back(p);
		            }});
		ControlServer ctl(sock, srv.session);
		auto s = ask(sock, "status");
		CHECK(field(s, "state") == "waiting" and field(s, "encryption") == "enabled" and field(s, "pin").empty());

		std::string connected_status, refresh_reply, bitrate_reply;
		std::atomic<bool> done{false};
		std::thread poller([&] {
			for (int i = 0; i < 400 and not done; ++i)
			{
				auto st = ask(sock, "status");
				if (field(st, "state") == "connected" and has(st, "\"battery\"") and not has(st, "\"frames\":0,"))
				{
					refresh_reply = ask(sock, "refresh 90");
					bitrate_reply = ask(sock, "bitrate 30");
					// the headset answers refresh_rate_change with refresh_rate_changed: wait for the round trip
					for (int j = 0; j < 80 and not has(connected_status, "\"refresh_hz\":90.0"); ++j)
					{
						connected_status = ask(sock, "status");
						std::this_thread::sleep_for(std::chrono::milliseconds(25));
					}
					done = true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
		});
		std::string key2 = crypto::key::generate_x448_keypair().private_key();
		auto r = run_fake_headset({.port = srv.session.port(),
		                           .frames = 150,
		                           .private_key = key2,
		                           .pin = "424242",
		                           .expect_h264 = true,
		                           .battery_charge = 0.5f,
		                           .start_app = "438100"});
		done = true;
		poller.join();
		CHECK(r.error.empty() and r.handshake_state == "pin_needed" and r.decoded[0] >= 150);
		CHECK(r.got_stream_description); // H.264 described
		CHECK(r.app_ids == std::vector<std::string>{"438100"});
		CHECK(r.refresh_changes == std::vector<float>{90.0f});
		CHECK(not has(refresh_reply, "\"error\"") and has(bitrate_reply, "\"target_mbps\":30.0"));
		CHECK(has(connected_status, "\"codec\":\"h264\"") and has(connected_status, "\"battery\":{\"charge\":0.50,\"charging\":true}"));
		CHECK(has(connected_status, "\"refresh_hz\":90.0"));
		CHECK(has(connected_status, "\"idr_requests\":"));
		CHECK(has(connected_status, "\"render_width\":320,\"render_height\":352")); // render scale 0.5 of 640x704
		printf("keyframe interval 0.25 s: %d IDR on eye 0 for %d frames\n", r.idr[0], r.decoded[0]);
		CHECK(r.idr[0] >= 4);                                                         // ~2 s of frames, IDR every 0.25 s
		CHECK(has(connected_status, "\"app_request\":{\"id\":\"438100\",\"seq\":1}"));
		srv.wait_disconnect();
		{
			std::lock_guard l(m);
			CHECK((pins == std::vector<std::string>{"424242", ""}));
		}
		s = ask(sock, "status");
		CHECK(field(s, "encryption") == "enabled" and field(s, "pin").empty() and has(s, "\"pairing_request\":null"));
		auto again = run_fake_headset({.port = srv.session.port(), .frames = 10, .private_key = key2, .expect_h264 = true});
		CHECK(again.error.empty() and again.handshake_state == "client_already_paired");
		srv.wait_disconnect();
		CHECK(has(ask(sock, "headsets"), "\"headsets\":[{\"name\":\"steamac fake headset\""));
	}
	{
		// encryption disabled: no pairing to switch on
		Server srv({.encryption = WivrnConfig::Encryption::disabled, .known_keys_file = keys_file});
		ControlServer ctl(sock, srv.session);
		CHECK(field(ask(sock, "status"), "encryption") == "disabled");
		CHECK(has(ask(sock, "pair on"), "\"error\""));
	}
	std::filesystem::remove_all(dir);
	return test_result("control");
}
