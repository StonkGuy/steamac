// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: a TCP connection to a running WivrnSession (encryption off, TCP only, loopback). The input is
// the raw byte stream the headset side sends (u32 length + payload frames); the harness then half-closes and
// waits until the session has dropped the connection. Covers the handshake (expect<>), headset_info handling
// (encoder setup) and every post-handshake packet handler.
#include "session/vr_session.h"
#include "wivrn/wivrn_session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
struct Listener final : vrhost::SessionListener
{
	void on_input(std::span<const vrhost::InputValue> v) override
	{
		for (auto & i: v)
			sink += i.value;
	}
	void on_tracking(std::span<const vrhost::TrackedPose> p) override { sink += p.size(); }
	void on_microphone(std::span<const int16_t> pcm, vrhost::ns) override
	{
		for (auto s: pcm)
			sink += s;
	}
	volatile double sink = 0;
};

vrhost::WivrnSession * session()
{
	static Listener listener;
	static vrhost::WivrnSession * s = [] {
		vrhost::WivrnConfig c;
		c.port = 0;
		c.advertise = false;
		c.tcp_only = true;
		c.encryption = vrhost::WivrnConfig::Encryption::disabled;
		c.known_keys_file = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/vrhost-fuzz-known-keys";
		c.applications = {{"steam", "Steam"}};
		c.log = [](const std::string & m) {
			if (getenv("VRHOST_FUZZ_LOG"))
				fprintf(stderr, "vrhost: %s\n", m.c_str());
		};
		auto * s = new vrhost::WivrnSession(c);
		s->start(&listener);
		return s;
	}();
	return s;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size)
{
	int port = session()->port();
	sockaddr_in6 a{};
	a.sin6_family = AF_INET6;
	a.sin6_port = htons(port);
	a.sin6_addr = in6addr_loopback;
	// thousands of short connections: close with a reset, and wait (up to 30 s) while the ephemeral ports are held in
	// TIME_WAIT by the session's side (it closes first)
	int fd = -1;
	for (int tries = 0;; ++tries)
	{
		fd = socket(AF_INET6, SOCK_STREAM, 0);
		linger lg{1, 0};
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
		if (connect(fd, (sockaddr *)&a, sizeof a) == 0)
			break;
		if (errno != EAGAIN or tries == 3000)
		{
			perror("fuzz_session: connect");
			abort();
		}
		close(fd);
		usleep(10000);
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	size_t off = 0;
	while (off < size)
	{
		ssize_t n = send(fd, data + off, size - off, 0);
		if (n <= 0)
			break;
		off += n;
	}
	shutdown(fd, SHUT_WR);
	// wait for the session to close the connection (it reads to EOF or throws); drain what it sends
	char sink[65536];
	for (;;)
	{
		pollfd p{fd, POLLIN, 0};
		if (poll(&p, 1, 20000) <= 0)
		{
			fprintf(stderr, "fuzz_session: the session did not close the connection within 20 s\n");
			abort();
		}
		if (recv(fd, sink, sizeof sink, 0) <= 0)
			break;
	}
	close(fd);
	return 0;
}
