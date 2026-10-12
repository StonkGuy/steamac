// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_channel.h"

#include "vd/vd_wire.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace vrhost::vd
{
Channel::Channel(int fd) : sock(fd) {}

Channel::~Channel()
{
	shutdown();
	close(sock);
}

void Channel::encrypt(const AesKey & k)
{
	std::lock_guard lock(send_mutex);
	enc = std::make_unique<CbcStream>(k, true);
	dec = std::make_unique<CbcStream>(k, false);
}

void Channel::shutdown()
{
	if (not dead.exchange(true))
		::shutdown(sock, SHUT_RDWR);
}

bool Channel::send(std::span<const uint8_t> msg)
{
	std::lock_guard lock(send_mutex);
	if (dead)
		return false;
	std::vector<uint8_t> wire;
	std::span<const uint8_t> out = msg;
	if (enc)
	{
		std::vector<uint8_t> chunk;
		put_varint(chunk, uint32_t(msg.size()));
		chunk.insert(chunk.end(), msg.begin(), msg.end());
		size_t pad = (16 - chunk.size() % 16) % 16;
		chunk.resize(chunk.size() + pad);
		random_bytes(std::span(chunk).last(pad));
		wire = enc->update(chunk);
		out = wire;
	}
	size_t done = 0;
	while (done < out.size())
	{
		ssize_t n = ::send(sock, out.data() + done, out.size() - done, 0);
		if (n < 0 and errno == EINTR)
			continue;
		if (n <= 0)
		{
			dead = true;
			return false;
		}
		done += size_t(n);
	}
	sent += out.size();
	return true;
}

bool Channel::recv_raw(uint8_t * out, size_t n, int timeout_ms)
{
	size_t got = 0;
	while (got < n)
	{
		if (dead)
			return false;
		pollfd p{sock, POLLIN, 0};
		int r = poll(&p, 1, timeout_ms > 0 ? timeout_ms : 200);
		if (r < 0 and errno != EINTR)
			return false;
		if (r <= 0)
		{
			if (timeout_ms > 0)
				return false;
			continue;
		}
		ssize_t k = recv(sock, out + got, n - got, 0);
		if (k < 0 and errno == EINTR)
			continue;
		if (k <= 0)
		{
			dead = true;
			return false;
		}
		got += size_t(k);
	}
	return true;
}

bool Channel::next_chunk(int timeout_ms)
{
	uint8_t first[16];
	if (not recv_raw(first, 16, timeout_ms))
		return false;
	auto block = dec->update(first);
	size_t used = 0;
	auto len = get_varint(block, used);
	if (not len)
		return false;
	size_t total = used + *len;
	total += (16 - total % 16) % 16;
	std::vector<uint8_t> rest(total - 16);
	if (not rest.empty())
	{
		if (not recv_raw(rest.data(), rest.size(), timeout_ms))
			return false;
		auto more = dec->update(rest);
		block.insert(block.end(), more.begin(), more.end());
	}
	plain.erase(plain.begin(), plain.begin() + plain_pos);
	plain_pos = 0;
	plain.insert(plain.end(), block.begin() + used, block.begin() + used + *len);
	return true;
}

bool Channel::read_exact(void * out, size_t n, int timeout_ms)
{
	auto o = static_cast<uint8_t *>(out);
	if (not dec)
		return recv_raw(o, n, timeout_ms);
	while (plain.size() - plain_pos < n)
		if (not next_chunk(timeout_ms))
			return false;
	memcpy(o, plain.data() + plain_pos, n);
	plain_pos += n;
	return true;
}

void tune_socket(int fd, bool keepalive)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	linger l{1, 0};
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
	timeval tv{20, 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (keepalive)
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
}

static int bind_any(int type, uint16_t port, uint16_t & bound)
{
	int fd = socket(AF_INET6, type, 0);
	if (fd < 0)
		throw std::runtime_error(std::string("vd: socket: ") + strerror(errno));
	int zero = 0, one = 1;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in6 a{};
	a.sin6_family = AF_INET6;
	a.sin6_port = htons(port);
	a.sin6_addr = in6addr_any;
	if (bind(fd, (sockaddr *)&a, sizeof a) != 0)
	{
		int e = errno;
		close(fd);
		throw std::runtime_error("vd: bind port " + std::to_string(port) + ": " + strerror(e));
	}
	socklen_t len = sizeof a;
	getsockname(fd, (sockaddr *)&a, &len);
	bound = ntohs(a.sin6_port);
	return fd;
}

int listen_tcp(uint16_t port, uint16_t & bound)
{
	int fd = bind_any(SOCK_STREAM, port, bound);
	if (listen(fd, 4) != 0)
	{
		close(fd);
		throw std::runtime_error("vd: listen failed");
	}
	return fd;
}

int bind_udp(uint16_t port, uint16_t & bound)
{
	int fd = bind_any(SOCK_DGRAM, port, bound);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	return fd;
}

int connect_tcp(const std::string & host, uint16_t port)
{
	sockaddr_storage ss{};
	socklen_t len = 0;
	if (auto a4 = (sockaddr_in *)&ss; inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1)
	{
		a4->sin_family = AF_INET;
		a4->sin_port = htons(port);
		len = sizeof *a4;
	}
	else if (auto a6 = (sockaddr_in6 *)&ss; inet_pton(AF_INET6, host.c_str(), &a6->sin6_addr) == 1)
	{
		a6->sin6_family = AF_INET6;
		a6->sin6_port = htons(port);
		len = sizeof *a6;
	}
	else
		return -1;
	int fd = socket(ss.ss_family, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (sockaddr *)&ss, len) != 0)
	{
		close(fd);
		return -1;
	}
	tune_socket(fd, false);
	return fd;
}
} // namespace vrhost::vd
