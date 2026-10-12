// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: LAN discovery datagrams and the computer record (VD-FACT(discovery-cipher, connect-now,
// offer-layout, rsa-padding, reply-layout, record-schema) in vd_assumptions.h). Pure encode/decode, shared by
// VdSession and the test peer; the ciphers are in vd_crypto.h.
//
//   headset -> host  offer: chunk_wrap(discovery key, RSA key XML (243 chars) [+ 0x00 platform] + account)
//   host -> headset  reply: RSA-OAEP(wrapper key + IV) (128 B) + chunk_wrap(wrapper key, record XML)
//   headset -> host  connect now (plain, 17 B): record connection id + platform
//   headset -> host  TCP to the fixed ports; channels use the record's Key/IV
#pragma once

#include "vd/vd_assumptions.h"
#include "vd/vd_crypto.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vrhost::vd
{
struct Offer
{
	std::string rsa_public_xml;      // exactly offer_key_chars
	std::optional<uint8_t> platform; // absent: Oculus (0)
	std::string account;
};
std::vector<uint8_t> encode_offer(const Offer &); // plaintext
std::optional<Offer> parse_offer(std::span<const uint8_t> plain);
std::vector<uint8_t> wrap_offer(const AesKey & discovery, const Offer &);
std::optional<Offer> unwrap_offer(const AesKey & discovery, std::span<const uint8_t> datagram);

struct ConnectNow
{
	std::array<uint8_t, 16> connection_id{};
	uint8_t platform = 0;
};
std::vector<uint8_t> encode_connect_now(const ConnectNow &);
std::optional<ConnectNow> parse_connect_now(std::span<const uint8_t> datagram);

struct NetAdapter
{
	std::string ip, mac; // mac: 12 upper-case hex digits, no separators (empty when unknown)
	bool wireless = false, gigabit = false, usb = false;
};
struct ComputerRecord
{
	std::string id, name, description;
	std::string os = os_windows;
	std::array<int, 4> version{streamer_version[0], streamer_version[1], streamer_version[2], streamer_version[3]};
	std::vector<NetAdapter> adapters;
	bool allow_remote = false, encrypt_local = true, encrypt_remote = true;
	std::array<uint8_t, 16> connection_id{};
	AesKey channel_key; // the key/IV of the TCP channels
	Region region = Region::europe;
};
std::string encode_record(const ComputerRecord &);
std::optional<ComputerRecord> parse_record(const std::string & xml);

// Reply to an approved offer (nullopt: the offer's RSA key is unusable or not 1024-bit).
std::optional<std::vector<uint8_t>> encode_reply(const std::string & rsa_public_xml, const ComputerRecord &);
// Test peer side.
std::optional<ComputerRecord> parse_reply(std::span<const uint8_t> datagram,
                                          const std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t>)> & rsa_decrypt);
} // namespace vrhost::vd
