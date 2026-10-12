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

// A continuous AES-256-CBC chain without padding (one per channel and direction, VD-FACT(stream-cipher)).
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

// The stream-chunk format on a byte buffer (discovery offer and reply, VD-FACT(discovery-cipher)): one CBC chain from
// the IV over chunks of 7-bit varint length + payload + random padding to 16 bytes.
std::vector<uint8_t> chunk_wrap(const AesKey &, std::span<const std::vector<uint8_t>> payloads);
// All chunk payloads concatenated; nullopt when the buffer is not block aligned or a chunk length does not fit.
std::optional<std::vector<uint8_t>> chunk_unwrap(const AesKey &, std::span<const uint8_t> wrapped);

// RSA public key given as .NET XML text (<RSAKeyValue><Modulus>b64</Modulus><Exponent>b64</Exponent>...).
// Encrypts with OAEP (SHA-1, VD-FACT(rsa-padding)); nullopt when the key does not parse.
std::optional<std::vector<uint8_t>> rsa_encrypt(const std::string & xml_public_key, std::span<const uint8_t> data);

// Test peer side: a fresh key pair, its public half as XML, decryption.
class RsaKeyPair
{
public:
	explicit RsaKeyPair(int bits = 1024);
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
