// SPDX-License-Identifier: GPL-3.0-or-later
#include "control/control_server.h"

#include "session/clock.h"
#include "session/frames_in_flight.h"
#include "wivrn/known_keys.h"
#include "wivrn/wivrn_session.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace vrhost
{
std::string ControlServer::json_string(const std::string & s)
{
	std::string out = "\"";
	for (unsigned char c: s)
	{
		if (c == '"' or c == '\\')
			out += '\\', out += char(c);
		else if (c < 0x20)
		{
			char b[8];
			snprintf(b, sizeof b, "\\u%04x", c);
			out += b;
		}
		else
			out += char(c);
	}
	return out + "\"";
}

ControlServer::ControlServer(std::string path_, WivrnSession & s, Hooks h) :
        path(std::move(path_)), session(s), hooks(std::move(h))
{
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	if (path.size() >= sizeof a.sun_path)
		throw std::runtime_error("control socket path too long: " + path);
	strcpy(a.sun_path, path.c_str());
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		throw std::runtime_error(std::string("control socket: ") + strerror(errno));
	unlink(path.c_str()); // a stale socket of an earlier host (one host per user at a time)
	mode_t old = umask(0077);
	int rc = bind(fd, (sockaddr *)&a, sizeof a);
	umask(old);
	if (rc != 0 or listen(fd, 4) != 0)
	{
		int e = errno;
		close(fd);
		throw std::runtime_error("control socket " + path + ": " + strerror(e));
	}
	thread = std::thread([this] { loop(); });
}

ControlServer::~ControlServer()
{
	quit = true;
	if (thread.joinable())
		thread.join();
	close(fd);
	unlink(path.c_str());
}

void ControlServer::loop()
{
	while (not quit)
	{
		pollfd p{fd, POLLIN, 0};
		if (poll(&p, 1, 200) != 1)
			continue;
		int c = accept(fd, nullptr, nullptr);
		if (c < 0)
			continue;
		timeval tv{1, 0};
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		int one = 1;
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
		std::string req;
		char buf[256];
		while (req.find('\n') == std::string::npos and req.size() < 1024)
		{
			ssize_t n = recv(c, buf, sizeof buf, 0);
			if (n <= 0)
				break;
			req.append(buf, n);
		}
		if (auto nl = req.find('\n'); nl != std::string::npos)
			req.resize(nl);
		std::string rep = handle(req) + "\n";
		(void)send(c, rep.data(), rep.size(), 0);
		close(c);
	}
}

std::string ControlServer::handle(const std::string & req)
{
	auto error = [](const std::string & what) {
		return "{\"v\":" + std::to_string(protocol_version) + ",\"error\":" + json_string(what) + "}";
	};
	if (req == "status")
		return status();
	if (req == "headsets")
		return headsets();
	if (req == "pair on" or req == "pair off")
	{
		bool on = req == "pair on";
		if (not session.set_pairing(on))
			return error("encryption is disabled: no pairing");
		if (hooks.pin_changed)
			hooks.pin_changed(session.pin());
		return status();
	}
	if (req.starts_with("frames-in-flight "))
	{
		char * end = nullptr;
		long n = req.substr(17) == "auto" ? long(FramesInFlight::adaptive) : strtol(req.c_str() + 17, &end, 10);
		if ((end and *end) or (n != long(FramesInFlight::adaptive) and (n < 0 or n > 8)))
			return error("frames-in-flight: 0 (off) to 8, or auto");
		if (not hooks.set_frames_in_flight or not hooks.set_frames_in_flight(uint32_t(n)))
			return error("frames-in-flight: no game link");
		return status();
	}
	if (req.starts_with("bitrate "))
	{
		// total Mbit/s, the range Settings > VR offers
		char * end = nullptr;
		long mbps = strtol(req.c_str() + 8, &end, 10);
		if (*end or mbps < 1 or mbps > 400)
			return error("bitrate: Mbit/s, 1 to 400");
		session.set_bitrate(uint32_t(mbps) * 1'000'000u);
		return status();
	}
	if (req.starts_with("refresh "))
	{
		char * end = nullptr;
		float hz = strtof(req.c_str() + 8, &end);
		if (*end or not (hz > 0))
			return error("refresh: rate in Hz");
		if (not session.request_refresh_rate(hz))
			return error("refresh: no headset connected, or it does not offer this rate");
		return status();
	}
	if (req == "unpair-all" or req.starts_with("unpair "))
	{
		std::string name = req == "unpair-all" ? "" : req.substr(7);
		if (req != "unpair-all" and name.empty())
			return error("unpair: no name");
		KnownKeys(session.known_keys_file()).remove(name);
		return headsets();
	}
	return error("unknown request");
}

std::string ControlServer::headsets()
{
	std::string out = "{\"v\":" + std::to_string(protocol_version) + ",\"headsets\":[";
	bool first = true;
	for (auto & k: KnownKeys(session.known_keys_file()).list())
	{
		out += first ? "" : ",";
		first = false;
		out += "{\"name\":" + json_string(k.name) + ",\"last_connection\":" + std::to_string(k.last_connection) + "}";
	}
	return out + "]}";
}

std::string ControlServer::status()
{
	using Enc = WivrnConfig::Encryption;
	const auto & s = session.stats();
	ns now = now_ns();
	uint64_t bytes = s.bytes_sent.load();
	if (last_sample_ns and now - last_sample_ns > 100'000'000)
		last_mbps = (bytes - last_bytes) * 8.0 / ((now - last_sample_ns) / 1e9) / 1e6;
	if (not last_sample_ns or now - last_sample_ns > 100'000'000)
	{
		last_sample_ns = now;
		last_bytes = bytes;
	}
	auto info = session.connected() ? session.headset() : std::nullopt;
	Enc enc = session.encryption();
	std::string pin = session.pin();
	const char * state = info ? "connected" : enc == Enc::pairing ? "pairing" : "waiting";
	char num[512];
	std::string out = "{\"v\":" + std::to_string(protocol_version) + ",\"state\":\"" + state + "\"";
	out += ",\"port\":" + std::to_string(session.port());
	out += ",\"bonjour\":" + json_string(session.advertised_name());
	out += std::string(",\"encryption\":\"") + (enc == Enc::disabled ? "disabled" : enc == Enc::pairing ? "pairing" : "enabled") + "\"";
	out += ",\"pin\":" + json_string(pin);
	if (info)
	{
		std::string rates;
		for (float r: info->refresh_rates)
		{
			snprintf(num, sizeof num, "%s%.0f", rates.empty() ? "" : ",", r);
			rates += num;
		}
		float current = session.refresh_rate();
		snprintf(num, sizeof num,
		         ",\"headset\":{\"name\":%s,\"eye_width\":%u,\"eye_height\":%u,\"render_width\":%u,\"render_height\":%u,"
		         "\"refresh_hz\":%.1f,\"refresh_rates\":[%s],\"codec\":\"%s\"",
		         json_string(info->name).c_str(), info->eye_width, info->eye_height, info->render_eye_width,
		         info->render_eye_height, current > 0 ? current : info->preferred_refresh_rate, rates.c_str(),
		         session.codec_name().c_str());
		out += num;
		if (auto b = session.battery())
		{
			snprintf(num, sizeof num, ",\"battery\":{\"charge\":%.2f,\"charging\":%s}", b->charge, b->charging ? "true" : "false");
			out += num;
		}
		out += "}";
	}
	else
		out += ",\"headset\":null";
	if (auto r = session.pairing_request())
		out += ",\"pairing_request\":{\"name\":" + json_string(r->name) + ",\"at\":" + std::to_string(r->at) +
		       ",\"refused\":" + (r->refused ? "true" : "false") + "}";
	else
		out += ",\"pairing_request\":null";
	auto app = session.app_request();
	out += ",\"app_request\":{\"id\":" + json_string(app.id) + ",\"seq\":" + std::to_string(app.seq) + "}";
	double pipeline = hooks.pipeline_latency_ms ? hooks.pipeline_latency_ms() : 0;
	snprintf(num, sizeof num,
	         ",\"stream\":{\"frames\":%" PRIu64 ",\"bitrate_mbps\":%.1f,\"target_mbps\":%.1f,\"encode_ms\":%.2f,"
	         "\"decode_ms\":%.2f,\"pipeline_ms\":%.2f",
	         s.frames_sent.load(), last_mbps, session.bitrate_bps() / 1e6, s.last_encode_ns / 1e6,
	         s.last_feedback_decode_ns / 1e6, pipeline);
	out += num;
	snprintf(num, sizeof num, ",\"idr_requests\":%" PRIu64 "}", s.idr_requests.load());
	out += (hooks.stream_json ? hooks.stream_json() : std::string()) + num;
	return out + "}";
}
} // namespace vrhost
