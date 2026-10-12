// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: LAN discovery datagrams and the computer record (PROTOCOL.md sections 2 and 4). Pure
// encode/decode, shared by VdSession and the test peer; the datagram cipher is in vd_crypto.h.
#pragma once

#include "vd/vd_assumptions.h"
#include "vd/vd_crypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vrhost::vd
{
// Shape 2: the headset's offer (after unwrapping): RSA public key XML + optional platform + account id.
struct Offer
{
	std::string rsa_public_xml;
	std::optional<uint16_t> platform;
	std::string account;
};
std::vector<uint8_t> encode_offer(const Offer &);
std::optional<Offer> parse_offer(std::span<const uint8_t> plain);

// Shape 1: "connect now" for the computer's current connection GUID.
struct ConnectNow
{
	std::array<uint8_t, 16> guid{};
	uint8_t platform = 0;
};
std::vector<uint8_t> encode_connect_now(const ConnectNow &);
std::optional<ConnectNow> parse_connect_now(std::span<const uint8_t> plain);

struct ComputerRecord
{
	std::string id; // connection GUID, text form
	std::string name, description, os = os_windows, version = streamer_version;
	std::vector<std::string> addresses; // private addresses, numeric
	uint16_t control_port = 0, data_port = 0, video_port = 0, audio_port = 0, discovery_port = 0;
	bool encrypt_local = true, encrypt_remote = true, allow_remote = false;
};
std::string encode_record(const ComputerRecord &);
std::optional<ComputerRecord> parse_record(const std::string & xml);

// Reply to an approved offer: RSA(session key + IV) then the record wrapped with the discovery key.
std::optional<std::vector<uint8_t>> encode_reply(const AesKey & discovery, const AesKey & session,
                                                 const std::string & rsa_public_xml, const ComputerRecord &);
// Test peer: rsa_size = modulus bytes of its key.
struct Reply
{
	AesKey session;
	ComputerRecord record;
};
template <typename Decrypt>
std::optional<Reply> parse_reply(const AesKey & discovery, std::span<const uint8_t> datagram, size_t rsa_size,
                                 Decrypt && rsa_decrypt);

std::string guid_text(const std::array<uint8_t, 16> &);

// ---
template <typename Decrypt>
std::optional<Reply> parse_reply(const AesKey & discovery, std::span<const uint8_t> d, size_t rsa_size,
                                 Decrypt && rsa_decrypt)
{
	if (d.size() <= rsa_size)
		return std::nullopt;
	std::optional<std::vector<uint8_t>> k = rsa_decrypt(d.first(rsa_size));
	if (not k or k->size() != 48)
		return std::nullopt;
	Reply r;
	std::copy_n(k->begin(), 32, r.session.key.begin());
	std::copy_n(k->begin() + 32, 16, r.session.iv.begin());
	auto rec = aes_unwrap(discovery, d.subspan(rsa_size));
	if (not rec)
		return std::nullopt;
	auto parsed = parse_record(std::string(rec->begin(), rec->end()));
	if (not parsed)
		return std::nullopt;
	r.record = *parsed;
	return r;
}
} // namespace vrhost::vd
