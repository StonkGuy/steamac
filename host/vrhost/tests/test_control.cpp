// SPDX-License-Identifier: GPL-3.0-or-later
// Control socket (docs/vr-interfaces.md section 5) over a real AF_UNIX socket against a loopback session: status
// while waiting, pairing switched on at run time (fresh PIN) and a new headset pairing with exactly that PIN, the
// connected headset's info and stream counters in the status, the paired list, unpair, pairing off, bad requests.
#include "check.h"
#include "control/control_server.h"
#include "wivrn/wivrn_control.h"
#include "fake_headset.h"
#include "pattern_server.h"

#include <csignal>
#include <filesystem>
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
		WivrnControl wc(srv.session);
		ControlServer ctl(sock, wc);
		CHECK(ControlServer::json_string("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");

		auto s = ask(sock, "status");
		CHECK(has(s, "\"v\":1") and field(s, "state") == "waiting" and field(s, "encryption") == "enabled");
		CHECK(field(s, "pin").empty() and has(s, "\"headset\":null"));
		CHECK(has(s, "\"port\":" + std::to_string(srv.session.port())));
		CHECK(field(s, "adapter") == "wivrn");

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

		CHECK(has(ask(sock, "bogus"), "\"error\":\"unknown request\""));
		CHECK(has(ask(sock, "unpair "), "\"error\""));
	}
	CHECK(not std::filesystem::exists(sock)); // removed with the server
	{
		// encryption disabled: no pairing to switch on
		Server srv({.encryption = WivrnConfig::Encryption::disabled, .known_keys_file = keys_file});
		WivrnControl wc(srv.session);
		ControlServer ctl(sock, wc);
		CHECK(field(ask(sock, "status"), "encryption") == "disabled");
		CHECK(has(ask(sock, "pair on"), "\"error\""));
	}
	std::filesystem::remove_all(dir);
	return test_result("control");
}
