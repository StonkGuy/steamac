// SPDX-License-Identifier: GPL-3.0-or-later
// Pins the WiVRn protocol: the hash must match what WiVRn v26.9 clients send (verified against the real client,
// docs/vr.md). A re-vendor of third_party/wivrn that changes it must update this constant and the docs.
#include "check.h"
#include "protocol_version.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"
#include "wivrn/wivrn_session.h"

int main()
{
	constexpr uint64_t wivrn_v26_9 = 0xfc26e619b7ab80f8ull;
	CHECK(wivrn::protocol_version == wivrn_v26_9);
	CHECK(vrhost::WivrnSession::protocol_version() == wivrn_v26_9);

	// A first shard with view_info and identity foveation round-trips through WiVRn's serializer.
	namespace th = wivrn::to_headset;
	std::vector<uint8_t> payload(1000, 0x5a);
	th::video_stream_data_shard s{};
	s.stream_item_idx = 1;
	s.frame_idx = 42;
	s.shard_idx = 0;
	th::video_stream_data_shard::view_info_t vi{};
	vi.display_time = 123456789;
	vi.pose[0].orientation.w = 1;
	vi.pose[1].orientation.w = 1;
	vi.foveation[0] = {{1832}, {1920}};
	vi.foveation[1] = {{1832}, {1920}};
	s.view_info = vi;
	s.payload = payload;

	// serialization_packet keeps references into the serialized object: keep it alive
	th::packets variant{s};
	wivrn::serialization_packet sp;
	sp.serialize(variant);
	std::vector<uint8_t> flat;
	for (auto span: (std::vector<std::span<uint8_t>> &)sp)
		flat.insert(flat.end(), span.begin(), span.end());
	auto buf = std::make_shared<uint8_t[]>(flat.size());
	memcpy(buf.get(), flat.data(), flat.size());
	wivrn::deserialization_packet dp{buf, std::span<uint8_t>(buf.get(), flat.size())};
	auto back = dp.deserialize<th::packets>();
	auto * r = std::get_if<th::video_stream_data_shard>(&back);
	REQUIRE(r);
	CHECK(r->frame_idx == 42 and r->stream_item_idx == 1);
	REQUIRE(r->view_info);
	CHECK(r->view_info->display_time == 123456789);
	CHECK(r->view_info->foveation[0].x.size() == 1 and r->view_info->foveation[0].x[0] == 1832);
	CHECK(r->payload.size() == payload.size() and r->payload[999] == 0x5a);

	// WiVRn's shard budget: payload = 1400 - serialized view_info; must leave room for a sensible payload.
	size_t vi_size = wivrn::serialized_size(s.view_info);
	CHECK(vi_size < 400);
	return test_result("protocol");
}
