// SPDX-License-Identifier: GPL-3.0-or-later
// Writes seed inputs for fuzz_packets (one serialized headset message each) and fuzz_session (a framed byte
// stream: a well-formed encryption-off handshake followed by post-handshake messages).
#include "protocol_version.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <cstdio>
#include <string>
#include <vector>

namespace fh = wivrn::from_headset;

static std::vector<uint8_t> ser(const fh::packets & v)
{
	wivrn::serialization_packet sp;
	sp.serialize(v);
	std::vector<uint8_t> out;
	for (auto s: (std::vector<std::span<uint8_t>> &)sp)
		out.insert(out.end(), s.begin(), s.end());
	return out;
}

static void frame(std::vector<uint8_t> & stream, const std::vector<uint8_t> & msg)
{
	uint32_t n = msg.size();
	stream.insert(stream.end(), (uint8_t *)&n, (uint8_t *)&n + 4);
	stream.insert(stream.end(), msg.begin(), msg.end());
}

static void write(const std::string & path, const std::vector<uint8_t> & b)
{
	FILE * f = fopen(path.c_str(), "wb");
	fwrite(b.data(), 1, b.size(), f);
	fclose(f);
}

int main(int argc, char ** argv)
{
	if (argc < 3)
		return 2;
	std::string pk = argv[1], ss = argv[2];
	fh::headset_info_packet info{};
	info.render_eye_width = info.stream_eye_width = 256;
	info.render_eye_height = info.stream_eye_height = 256;
	info.available_refresh_rates = {72, 90};
	info.settings.preferred_refresh_rate = 72;
	info.speaker = fh::headset_info_packet::audio_description{2, 48000};
	info.microphone = fh::headset_info_packet::audio_description{1, 48000};
	info.supported_codecs = {wivrn::h265, wivrn::h264};
	info.system_name = "seed";
	for (auto & f: info.fov)
		f = {-0.8f, 0.8f, 0.8f, -0.8f};
	fh::tracking tr{};
	tr.device_poses.push_back({});
	tr.device_poses.back().device = wivrn::device_id::HEAD;
	tr.device_poses.back().flags = fh::orientation_valid | fh::position_valid;
	fh::inputs in{};
	in.values.push_back({.id = wivrn::device_id::LEFT_TRIGGER_VALUE, .value = 0.5f});
	fh::feedback fb{};
	fb.frame_index = 1;
	fb.stream_index = 0;
	wivrn::audio_data ad{};
	std::vector<uint8_t> pcm(64, 1);
	ad.payload = pcm;
	std::vector<fh::packets> msgs{
	        fh::crypto_handshake{.protocol_version = wivrn::protocol_version, .name = "seed"},
	        fh::handshake{},
	        info,
	        tr,
	        in,
	        fb,
	        fh::timesync_response{.query = 1, .response = 2},
	        fh::settings_changed{.bitrate_bps = 1000000},
	        fh::refresh_rate_changed{.from = 72, .to = 90},
	        ad,
	        fh::battery{.charge = 0.5f, .present = true},
	        fh::get_application_list{.language = "en"},
	        fh::start_app{.app_id = "steam"},
	};
	for (size_t i = 0; i < msgs.size(); ++i)
		write(pk + "/m" + std::to_string(i), ser(msgs[i]));
	// session: crypto_handshake, crypto_handshake (confirm), handshake, headset_info, then the rest
	std::vector<uint8_t> base;
	frame(base, ser(msgs[0]));
	frame(base, ser(fh::crypto_handshake{}));
	frame(base, ser(msgs[1]));
	frame(base, ser(msgs[2]));
	write(ss + "/handshake", base);
	auto all = base;
	for (size_t i = 3; i < 13; ++i)
	{
		auto one = base;
		frame(one, ser(msgs[i]));
		write(ss + "/s" + std::to_string(i), one);
		frame(all, ser(msgs[i]));
	}
	write(ss + "/all", all);
	return 0;
}
