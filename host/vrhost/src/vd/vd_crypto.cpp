// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_crypto.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <stdexcept>

namespace vrhost::vd
{
void random_bytes(std::span<uint8_t> b)
{
	if (RAND_bytes(b.data(), int(b.size())) != 1)
		throw std::runtime_error("vd: RAND_bytes failed");
}

AesKey AesKey::random()
{
	AesKey k;
	random_bytes(k.key);
	random_bytes(k.iv);
	return k;
}

static std::optional<std::vector<uint8_t>> cbc_once(const AesKey & k, std::span<const uint8_t> in, bool encrypt)
{
	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
	if (not ctx or EVP_CipherInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, k.key.data(), k.iv.data(), encrypt) != 1)
		throw std::runtime_error("vd: AES init failed");
	std::vector<uint8_t> out(in.size() + 16);
	int n = 0, m = 0;
	if (EVP_CipherUpdate(ctx.get(), out.data(), &n, in.data(), int(in.size())) != 1 or
	    EVP_CipherFinal_ex(ctx.get(), out.data() + n, &m) != 1)
		return std::nullopt;
	out.resize(n + m);
	return out;
}

std::vector<uint8_t> aes_wrap(const AesKey & k, std::span<const uint8_t> plain)
{
	return *cbc_once(k, plain, true);
}

std::optional<std::vector<uint8_t>> aes_unwrap(const AesKey & k, std::span<const uint8_t> wrapped)
{
	if (wrapped.empty() or wrapped.size() % 16)
		return std::nullopt;
	return cbc_once(k, wrapped, false);
}

CbcStream::CbcStream(const AesKey & k, bool encrypt) : ctx(EVP_CIPHER_CTX_new())
{
	auto c = static_cast<EVP_CIPHER_CTX *>(ctx);
	if (not c or EVP_CipherInit_ex(c, EVP_aes_256_cbc(), nullptr, k.key.data(), k.iv.data(), encrypt) != 1)
		throw std::runtime_error("vd: AES init failed");
	EVP_CIPHER_CTX_set_padding(c, 0);
}

CbcStream::~CbcStream()
{
	EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX *>(ctx));
}

std::vector<uint8_t> CbcStream::update(std::span<const uint8_t> in)
{
	if (in.size() % 16)
		throw std::runtime_error("vd: CBC input not block aligned");
	std::vector<uint8_t> out(in.size() + 16);
	int n = 0;
	if (EVP_CipherUpdate(static_cast<EVP_CIPHER_CTX *>(ctx), out.data(), &n, in.data(), int(in.size())) != 1)
		throw std::runtime_error("vd: AES update failed");
	out.resize(n);
	return out;
}

std::string base64(std::span<const uint8_t> b)
{
	std::string out(4 * ((b.size() + 2) / 3) + 1, '\0');
	int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), b.data(), int(b.size()));
	out.resize(n);
	return out;
}

std::optional<std::vector<uint8_t>> unbase64(const std::string & s)
{
	if (s.empty() or s.size() % 4)
		return std::nullopt;
	std::vector<uint8_t> out(s.size() / 4 * 3);
	int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char *>(s.data()), int(s.size()));
	if (n < 0)
		return std::nullopt;
	// EVP_DecodeBlock keeps the bytes of the '=' padding
	size_t pad = s.ends_with("==") ? 2 : s.ends_with('=') ? 1 : 0;
	out.resize(n - pad);
	return out;
}

static std::string xml_element(const std::string & xml, const std::string & name)
{
	auto a = xml.find("<" + name + ">");
	if (a == std::string::npos)
		return "";
	a += name.size() + 2;
	auto b = xml.find("</" + name + ">", a);
	return b == std::string::npos ? "" : xml.substr(a, b - a);
}

std::optional<std::vector<uint8_t>> rsa_encrypt(const std::string & xml, std::span<const uint8_t> data)
{
	auto n = unbase64(xml_element(xml, "Modulus"));
	auto e = unbase64(xml_element(xml, "Exponent"));
	if (not n or not e)
		return std::nullopt;
	std::unique_ptr<BIGNUM, decltype(&BN_free)> bn_n(BN_bin2bn(n->data(), int(n->size()), nullptr), BN_free),
	        bn_e(BN_bin2bn(e->data(), int(e->size()), nullptr), BN_free);
	std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> bld(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
	if (not bn_n or not bn_e or not bld or not OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, bn_n.get()) or
	    not OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, bn_e.get()))
		return std::nullopt;
	std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(bld.get()), OSSL_PARAM_free);
	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> fctx(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr),
	                                                                 EVP_PKEY_CTX_free);
	EVP_PKEY * raw = nullptr;
	if (not params or not fctx or EVP_PKEY_fromdata_init(fctx.get()) != 1 or
	    EVP_PKEY_fromdata(fctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.get()) != 1)
		return std::nullopt;
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(raw, EVP_PKEY_free);
	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new(pkey.get(), nullptr), EVP_PKEY_CTX_free);
	size_t len = 0;
	if (not ctx or EVP_PKEY_encrypt_init(ctx.get()) != 1 or
	    EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) != 1 or
	    EVP_PKEY_encrypt(ctx.get(), nullptr, &len, data.data(), data.size()) != 1)
		return std::nullopt;
	std::vector<uint8_t> out(len);
	if (EVP_PKEY_encrypt(ctx.get(), out.data(), &len, data.data(), data.size()) != 1)
		return std::nullopt;
	out.resize(len);
	return out;
}

RsaKeyPair::RsaKeyPair(int bits) : pkey(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(bits)))
{
	if (not pkey)
		throw std::runtime_error("vd: RSA keygen failed");
}

RsaKeyPair::~RsaKeyPair()
{
	EVP_PKEY_free(static_cast<EVP_PKEY *>(pkey));
}

size_t RsaKeyPair::size() const
{
	return size_t(EVP_PKEY_get_size(static_cast<EVP_PKEY *>(pkey)));
}

std::string RsaKeyPair::public_xml() const
{
	auto k = static_cast<EVP_PKEY *>(pkey);
	BIGNUM *n = nullptr, *e = nullptr;
	EVP_PKEY_get_bn_param(k, OSSL_PKEY_PARAM_RSA_N, &n);
	EVP_PKEY_get_bn_param(k, OSSL_PKEY_PARAM_RSA_E, &e);
	auto b64 = [](BIGNUM * v) {
		std::vector<uint8_t> b(BN_num_bytes(v));
		BN_bn2bin(v, b.data());
		BN_free(v);
		return base64(b);
	};
	return "<RSAKeyValue><Modulus>" + b64(n) + "</Modulus><Exponent>" + b64(e) + "</Exponent></RSAKeyValue>";
}

std::optional<std::vector<uint8_t>> RsaKeyPair::decrypt(std::span<const uint8_t> in) const
{
	std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
	        EVP_PKEY_CTX_new(static_cast<EVP_PKEY *>(pkey), nullptr), EVP_PKEY_CTX_free);
	size_t len = 0;
	if (not ctx or EVP_PKEY_decrypt_init(ctx.get()) != 1 or
	    EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) != 1 or
	    EVP_PKEY_decrypt(ctx.get(), nullptr, &len, in.data(), in.size()) != 1)
		return std::nullopt;
	std::vector<uint8_t> out(len);
	if (EVP_PKEY_decrypt(ctx.get(), out.data(), &len, in.data(), in.size()) != 1)
		return std::nullopt;
	out.resize(len);
	return out;
}
} // namespace vrhost::vd
