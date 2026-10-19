// SPDX-License-Identifier: GPL-3.0-or-later
// WiVRn's crypto as built here (vendored common/crypto, smp, secrets on the static libcrypto): both peers derive the
// same AES keys, SMP accepts the right PIN and rejects a wrong one, and the paired-headset store.
#include "check.h"

#include "crypto.h"
#include "secrets.h"
#include "smp.h"
#include "wivrn/known_keys.h"

#include <cstring>
#include <filesystem>
#include <openssl/opensslv.h>
#include <unistd.h>

using namespace vrhost;

static bool smp_match(const std::string & headset_pin, const std::string & server_pin)
{
	crypto::smp headset, server;
	auto m1 = headset.step1(headset_pin);
	auto m2 = server.step2(m1, server_pin);
	auto m3 = headset.step3(m2);
	auto [m4, server_ok] = server.step4(m3);
	bool headset_ok = headset.step5(m4);
	CHECK(server_ok == headset_ok);
	return server_ok;
}

int main()
{
	printf("%s\n", OPENSSL_VERSION_TEXT);

	crypto::key headset = crypto::key::generate_x448_keypair();
	crypto::key server = crypto::key::generate_x448_keypair();
	crypto::key headset_pub = crypto::key::from_public_key(headset.public_key());
	crypto::key server_pub = crypto::key::from_public_key(server.public_key());
	secrets s_server{server, headset_pub, "123456"};
	secrets s_headset{headset, server_pub, "123456"};
	CHECK(memcmp(&s_server, &s_headset, sizeof(secrets)) == 0);
	secrets s_wrong{headset, server_pub, "654321"};
	CHECK(memcmp(&s_server.control_key, &s_wrong.control_key, 16) != 0);

	// the headset's private key survives a PEM round trip (the fake headset reconnects with it)
	crypto::key again = crypto::key::from_private_key(headset.private_key());
	CHECK(again.public_key() == headset.public_key());

	CHECK(smp_match("481516", "481516"));
	CHECK(not smp_match("481516", "481517"));

	std::string pin = KnownKeys::random_pin();
	CHECK(pin.size() == 6 and pin.find_first_not_of("0123456789") == std::string::npos);

	std::string clean = KnownKeys::clean_key(headset.public_key());
	CHECK(clean.find("BEGIN") == std::string::npos and clean.find('\n') == std::string::npos and not clean.empty());

	char dir[] = "/tmp/vrhost-crypto.XXXXXX";
	REQUIRE(mkdtemp(dir));
	KnownKeys keys(std::filesystem::path(dir) / "sub" / "known_keys");
	CHECK(keys.list().empty());
	keys.add({.public_key = clean, .name = "Quest\t3"});
	keys.add({.public_key = "other", .name = "Frame"});
	keys.add({.public_key = clean, .name = "Quest 3"}); // re-pairing replaces
	auto l = keys.list();
	CHECK(l.size() == 2);
	CHECK(keys.contains(clean) and not keys.contains("nope"));
	CHECK(l[1].name == "Quest 3" and l[1].last_connection > 0);
	CHECK((std::filesystem::status(keys.file()).permissions() & std::filesystem::perms::others_read) == std::filesystem::perms::none);
	CHECK(keys.remove("Frame") == 1);
	CHECK(keys.remove("") == 1);
	CHECK(keys.list().empty());
	std::filesystem::remove_all(dir);
	return test_result("crypto");
}
