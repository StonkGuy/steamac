// SPDX-License-Identifier: GPL-3.0-or-later
// Status/pairing control socket of steamac-vrhost (`--control-socket PATH`), for the launcher's VR panel.
// Contract: docs/vr-interfaces.md section 5. AF_UNIX SOCK_STREAM, one request line per connection, one JSON
// object line back, then close. Requests: "status", "headsets", "pair on", "pair off", "unpair NAME", "unpair-all",
// "bitrate MBPS", "refresh HZ".
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace vrhost
{
class WivrnSession;
class KnownKeys;

class ControlServer
{
public:
	struct Hooks
	{
		std::function<double()> pipeline_latency_ms;      // game frame submit → sent, p50 (0 = unknown)
		std::function<void(const std::string &)> pin_changed; // new PIN ("" = pairing off), e.g. for --show-pin
		// extra members of the status "stream" object (",\"key\":value..."), e.g. the game pipeline's counters
		std::function<std::string()> stream_json;
		std::function<bool(uint32_t)> set_frames_in_flight; // "frames-in-flight N|auto"; false = no game link
	};
	ControlServer(std::string path, WivrnSession &, Hooks = {});
	~ControlServer();
	ControlServer(const ControlServer &) = delete;
	ControlServer & operator=(const ControlServer &) = delete;

	// One request → one reply line (without the newline). Public for tests.
	std::string handle(const std::string & request);

	static std::string json_string(const std::string &);
	static constexpr int protocol_version = 1;

private:
	std::string path;
	WivrnSession & session;
	Hooks hooks;
	int fd = -1;
	std::atomic<bool> quit{false};
	std::thread thread;
	// measured bitrate between two status requests
	int64_t last_sample_ns = 0;
	uint64_t last_bytes = 0;
	double last_mbps = 0;

	void loop();
	std::string status();
	std::string headsets();
};
} // namespace vrhost
