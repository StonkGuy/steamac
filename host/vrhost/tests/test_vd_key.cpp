// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: the run-time reader of the discovery key (src/vd/vd_streamer_key.h) against a DUMMY
// fixture: a minimal .NET-style PE file this test builds itself (one class with a static constructor that fills a
// 32-byte and a 16-byte array from field-RVA data, plus a decoy 16-byte array before them). Never the real value.
#include "check.h"
#include "vd/vd_streamer_key.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vrhost::vd;

namespace
{
struct Image
{
	std::vector<uint8_t> b = std::vector<uint8_t>(0x800);
	void u8(size_t o, uint8_t v) { b[o] = v; }
	void u16(size_t o, uint16_t v) { memcpy(&b[o], &v, 2); }
	void u32(size_t o, uint32_t v) { memcpy(&b[o], &v, 4); }
	void raw(size_t o, const void * p, size_t n) { memcpy(&b[o], p, n); }
};

// Section .text: file offset == RVA == 0x200, 0x600 bytes.
std::vector<uint8_t> build_fixture(const uint8_t (&key)[32], const uint8_t (&iv)[16], const char * type_name)
{
	Image im;
	im.u16(0, 0x5a4d);
	im.u32(0x3c, 0x80);
	im.u32(0x80, 0x00004550);
	im.u16(0x84, 0x14c); // machine
	im.u16(0x86, 1);     // sections
	im.u16(0x94, 0xe0);  // optional header size (PE32)
	size_t opt = 0x98;
	im.u16(opt, 0x10b);
	im.u32(opt + 96 + 14 * 8, 0x200); // CLI header
	im.u32(opt + 96 + 14 * 8 + 4, 72);
	size_t sec = opt + 0xe0;
	im.raw(sec, ".text", 5);
	im.u32(sec + 8, 0x600);  // virtual size
	im.u32(sec + 12, 0x200); // VA
	im.u32(sec + 16, 0x600); // raw size
	im.u32(sec + 20, 0x200); // raw pointer
	// CLI header
	im.u32(0x200, 72);
	im.u32(0x208, 0x400); // metadata RVA
	im.u32(0x20c, 0x200);
	// .cctor body (tiny header): decoy 16-byte array (field 3), key (field 1), IV (field 2), ret
	std::vector<uint8_t> il;
	auto init = [&](uint8_t n, uint8_t field) {
		uint8_t seq[] = {0x1f, n, 0x8d, 1, 0, 0, 0x01, 0x25, 0xd0, field, 0, 0, 0x04, 0x28, 1, 0, 0, 0x0a, 0x26};
		il.insert(il.end(), seq, seq + sizeof seq);
	};
	init(16, 3);
	init(32, 1);
	init(16, 2);
	il.push_back(0x2a);
	im.u8(0x260, uint8_t(il.size() << 2 | 2));
	im.raw(0x261, il.data(), il.size());
	// field data
	im.raw(0x300, key, 32);
	im.raw(0x320, iv, 16);
	uint8_t decoy[16];
	memset(decoy, 0xee, 16);
	im.raw(0x330, decoy, 16);

	// metadata root
	size_t md = 0x400;
	im.u32(md, 0x424a5342);
	im.u16(md + 4, 1);
	im.u16(md + 6, 1);
	im.u32(md + 12, 12);
	im.raw(md + 16, "v4.0.30319", 10);
	size_t p = md + 16 + 12;
	im.u16(p, 0); // flags
	im.u16(p + 2, 2);
	p += 4;
	// strings heap
	std::string strings = std::string("\0<Module>\0", 10);
	uint16_t s_module = 1, s_type = uint16_t(strings.size());
	strings += type_name + std::string(1, '\0');
	uint16_t s_cctor = uint16_t(strings.size());
	strings += std::string(".cctor\0", 7);
	uint16_t s_field = uint16_t(strings.size());
	strings += std::string("f\0", 2);
	while (strings.size() % 4)
		strings += '\0';
	// tables stream
	std::vector<uint8_t> t;
	auto w16 = [&](uint16_t v) { t.push_back(uint8_t(v)), t.push_back(uint8_t(v >> 8)); };
	auto w32 = [&](uint32_t v) { w16(uint16_t(v)), w16(uint16_t(v >> 16)); };
	w32(0);
	t.push_back(2), t.push_back(0); // version
	t.push_back(0);                 // heap sizes
	t.push_back(1);
	uint64_t valid = 1ull << 0 | 1ull << 2 | 1ull << 4 | 1ull << 6 | 1ull << 0x1d;
	w32(uint32_t(valid)), w32(uint32_t(valid >> 32));
	w32(0), w32(0); // sorted
	w32(1), w32(2), w32(3), w32(1), w32(3); // rows: Module, TypeDef, Field, MethodDef, FieldRVA
	w16(0), w16(s_module), w16(0), w16(0), w16(0);                    // Module
	w32(0), w16(s_module), w16(0), w16(0), w16(1), w16(1);            // TypeDef <Module>
	w32(0), w16(s_type), w16(0), w16(0), w16(1), w16(1);              // TypeDef discovery class
	for (int i = 0; i < 3; ++i)
		w16(0x31), w16(s_field), w16(0);                              // Field x3
	w32(0x260), w16(0), w16(0x1811), w16(s_cctor), w16(0), w16(1);    // MethodDef .cctor
	w32(0x300), w16(1), w32(0x320), w16(2), w32(0x330), w16(3);       // FieldRVA
	while (t.size() % 4)
		t.push_back(0);
	size_t streams = p + 2 * 8 + 4 + 12; // two headers: "#~\0\0" and "#Strings\0\0\0\0"
	im.u32(p, uint32_t(streams - md));
	im.u32(p + 4, uint32_t(t.size()));
	im.raw(p + 8, "#~\0\0", 4);
	im.u32(p + 12, uint32_t(streams - md + t.size()));
	im.u32(p + 16, uint32_t(strings.size()));
	im.raw(p + 20, "#Strings\0\0\0\0", 12);
	im.raw(streams, t.data(), t.size());
	im.raw(streams + t.size(), strings.data(), strings.size());
	return im.b;
}

std::filesystem::path write(const std::filesystem::path & dir, const char * name, const std::vector<uint8_t> & b)
{
	auto p = dir / name;
	std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
	return p;
}
} // namespace

int main()
{
	char tmpl[] = "/tmp/vrhost-vdkey.XXXXXX";
	REQUIRE(mkdtemp(tmpl));
	std::filesystem::path dir = tmpl;
	uint8_t key[32], iv[16];
	for (int i = 0; i < 32; ++i)
		key[i] = uint8_t(0xa0 + i);
	for (int i = 0; i < 16; ++i)
		iv[i] = uint8_t(0x10 + i);

	auto good = write(dir, "good.dll", build_fixture(key, iv, "ComputerDiscoveryClient"));
	auto r = read_streamer_key(good);
	if (not r.key)
		fprintf(stderr, "reader: %s\n", r.error.c_str());
	REQUIRE(r.key);
	CHECK(memcmp(r.key->key.data(), key, 32) == 0); // the 32-byte array after the decoy
	CHECK(memcmp(r.key->iv.data(), iv, 16) == 0);   // the 16-byte array AFTER the key, not the decoy before it

	auto other = read_streamer_key(write(dir, "other.dll", build_fixture(key, iv, "SomethingElse")));
	CHECK(not other.key and other.error == "discovery class not found");
	auto bytes = build_fixture(key, iv, "ComputerDiscoveryClient");
	bytes.resize(0x300);
	CHECK(not read_streamer_key(write(dir, "cut.dll", bytes)).key);
	CHECK(not read_streamer_key(write(dir, "text.dll", std::vector<uint8_t>(100, 'x'))).key);
	CHECK(not read_streamer_key(dir / "missing.dll").key);
	// errors never carry key material
	CHECK(other.error.find(std::string(reinterpret_cast<const char *>(key), 4)) == std::string::npos);

	auto cands = streamer_assembly_candidates();
	CHECK(cands.size() >= 2 and cands[0].string().find("/Applications/Virtual Desktop Streamer.app/") == 0 and
	      cands[0].string().ends_with("osx-arm64/VirtualDesktop.Net.dll"));

	std::filesystem::remove_all(dir);
	return test_result("vd_key");
}
