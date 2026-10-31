// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.h"
#include "wivrn/annexb.h"
#include <vector>

using namespace vrhost;

int main()
{
	// two NALs, 4-byte lengths: VPS-like (type 32) and IDR slice (type 19)
	std::vector<uint8_t> avcc = {0, 0, 0, 3, 0x40, 0x01, 0xaa, 0, 0, 0, 2, 0x26, 0x01};
	std::vector<uint8_t> out;
	CHECK(annexb::append_length_prefixed(out, avcc, 4));
	std::vector<uint8_t> want = {0, 0, 0, 1, 0x40, 0x01, 0xaa, 0, 0, 0, 1, 0x26, 0x01};
	CHECK(out == want);

	auto nals = annexb::split(out);
	REQUIRE(nals.size() == 2);
	CHECK(annexb::hevc_nal_type(nals[0]) == annexb::hevc_vps);
	CHECK(annexb::hevc_nal_type(nals[1]) == annexb::hevc_idr_w_radl);
	CHECK(annexb::hevc_is_irap(annexb::hevc_nal_type(nals[1])));

	// 2-byte lengths
	out.clear();
	CHECK(annexb::append_length_prefixed(out, std::vector<uint8_t>{0, 2, 0x42, 0x01}, 2));
	CHECK(out.size() == 6);

	// malformed: length past the end, zero length, bad length size
	out.clear();
	CHECK(not annexb::append_length_prefixed(out, std::vector<uint8_t>{0, 0, 0, 9, 1}, 4));
	CHECK(not annexb::append_length_prefixed(out, std::vector<uint8_t>{0, 0, 0, 0}, 4));
	CHECK(not annexb::append_length_prefixed(out, std::vector<uint8_t>{0, 1, 1}, 3));

	// 3-byte start codes are accepted by split
	std::vector<uint8_t> three = {0, 0, 1, 0x44, 0x01, 0, 0, 1, 0x02, 0x01};
	auto n3 = annexb::split(three);
	REQUIRE(n3.size() == 2);
	CHECK(annexb::hevc_nal_type(n3[0]) == annexb::hevc_pps);
	CHECK(annexb::hevc_nal_type(n3[1]) == 1);
	return test_result("annexb");
}
