// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: one TCP channel with the optional AES stream wrapper (PROTOCOL.md section 3).
// Plain: messages go out as they are (each is self-delimiting). Encrypted: each send() is one chunk = 7-bit varint
// length + message + random padding to the AES block, encrypted in one continuous CBC chain per direction.
// read_exact() hides the chunking, so message parsers are the same in both modes. Used by the host and the test peer.
#pragma once

#include "vd/vd_crypto.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace vrhost::vd
{
class Channel
{
public:
	explicit Channel(int fd); // takes ownership
	~Channel();
	Channel(const Channel &) = delete;
	Channel & operator=(const Channel &) = delete;

	// Switch both directions to the AES wrapper from now on.
	void encrypt(const AesKey &);
	bool encrypted() const { return enc != nullptr; }
	// Whole message (thread-safe); false once the peer is gone.
	bool send(std::span<const uint8_t>);
	// Blocks until n bytes arrived; false on close, error, or when timeout_ms (> 0) passes without data.
	bool read_exact(void * out, size_t n, int timeout_ms = 0);
	// Wake a blocked reader and refuse further I/O.
	void shutdown();
	bool closed() const { return dead; }
	int fd() const { return sock; }
	uint64_t bytes_sent() const { return sent; }

private:
	int sock;
	std::mutex send_mutex;
	std::unique_ptr<CbcStream> enc, dec;
	std::vector<uint8_t> plain; // decrypted, not yet consumed
	size_t plain_pos = 0;
	std::atomic<bool> dead{false};
	std::atomic<uint64_t> sent{0};

	bool recv_raw(uint8_t * out, size_t n, int timeout_ms);
	bool next_chunk(int timeout_ms);
};

// Dual-stack (IPv6 any, IPv4 mapped) TCP listener; port 0 = ephemeral. Returns the fd, throws on error.
int listen_tcp(uint16_t port, uint16_t & bound_port);
// Dual-stack UDP socket bound to port (0 = ephemeral), broadcast allowed.
int bind_udp(uint16_t port, uint16_t & bound_port);
// Applies the notes' socket options (NoDelay, linger 0, 20 s send timeout, keep-alive when asked, no SIGPIPE).
void tune_socket(int fd, bool keepalive);
// Test peer: connect to host:port (numeric IPv4/IPv6), -1 on failure.
int connect_tcp(const std::string & host, uint16_t port);
} // namespace vrhost::vd
