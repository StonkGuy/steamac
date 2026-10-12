// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: the ciphers the link uses (OpenSSL libcrypto, already linked for WiVRn). Key material is
// never logged or stored: the discovery key comes from the user's own installed Streamer at run time, session keys
// are random per registration.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vrhost::vd
{
struct AesKey
{
	std::array<uint8_t, 32> key{};
	std::array<uint8_t, 16> iv{};
	static AesKey random();
};

void random_bytes(std::span<uint8_t>);

// One discovery datagram, AES-256-CBC + PKCS#7 (TODO(vd-live:discovery-cipher)).
std::vector<uint8_t> aes_wrap(const AesKey &, std::span<const uint8_t> plain);
std::optional<std::vector<uint8_t>> aes_unwrap(const AesKey &, std::span<const uint8_t> wrapped);

// A continuous AES-256-CBC chain without padding (one per channel and direction, TODO(vd-live:stream-cipher)).
class CbcStream
{
public:
	CbcStream(const AesKey &, bool encrypt);
	~CbcStream();
	CbcStream(const CbcStream &) = delete;
	CbcStream & operator=(const CbcStream &) = delete;
	// in.size() must be a multiple of 16
	std::vector<uint8_t> update(std::span<const uint8_t> in);

private:
	void * ctx;
};

// RSA public key given as .NET XML text (<RSAKeyValue><Modulus>b64</Modulus><Exponent>b64</Exponent>...).
// Encrypts with PKCS#1 v1.5 (TODO(vd-live:rsa-padding)); nullopt when the key does not parse.
std::optional<std::vector<uint8_t>> rsa_encrypt(const std::string & xml_public_key, std::span<const uint8_t> data);

// Test peer side: a fresh key pair, its public half as XML, decryption.
class RsaKeyPair
{
public:
	explicit RsaKeyPair(int bits = 2048);
	~RsaKeyPair();
	RsaKeyPair(const RsaKeyPair &) = delete;
	RsaKeyPair & operator=(const RsaKeyPair &) = delete;
	std::string public_xml() const;
	size_t size() const; // modulus bytes = ciphertext size
	std::optional<std::vector<uint8_t>> decrypt(std::span<const uint8_t>) const;

private:
	void * pkey;
};

std::string base64(std::span<const uint8_t>);
std::optional<std::vector<uint8_t>> unbase64(const std::string &);
} // namespace vrhost::vd
