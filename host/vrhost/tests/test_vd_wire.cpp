// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter, offline: pins every protocol fact of src/vd/vd_assumptions.h (a change there must change
// this test), and checks the wire primitives: big-endian control framing, varints, the AES stream wrapper on a socket
// pair, the tracking blob offsets, discovery datagrams and the exact computer record, settings objects, and the
// render-divisor pacing (A1).
#include "check.h"
#include "session/clock.h"
#include "vd/vd_channel.h"
#include "vd/vd_discovery.h"
#include "vd/vd_session.h"
#include "vd/vd_settings.h"
#include "vd/vd_wire.h"

#include <cmath>
#include <cstddef>
#include <sys/socket.h>
#include <thread>

using namespace vrhost;
using namespace vrhost::vd;

static void pinned_constants()
{
	// VD-FACT(ports)
	CHECK(default_control_port == 38810 and default_data_port == 38820 and default_video_port == 38830 and
	      default_audio_port == 38840 and default_discovery_port == 38850);
	VdConfig cfg;
	CHECK(cfg.control_port == 38810 and cfg.data_port == 38820 and cfg.video_port == 38830 and cfg.audio_port == 38840 and
	      cfg.discovery_port == 38850);
	// VD-FACT(control-ids)
	CHECK(uint8_t(Msg::peer_info) == 1 and uint8_t(Msg::update_required) == 4 and uint8_t(Msg::update_unavailable) == 5 and
	      uint8_t(Msg::reload_user_settings) == 11 and uint8_t(Msg::user_setting_changed) == 12 and
	      uint8_t(Msg::reload_mobile_settings) == 13 and uint8_t(Msg::mobile_setting_changed) == 14 and
	      uint8_t(Msg::reload_streamer_settings) == 15 and uint8_t(Msg::streamer_setting_changed) == 16 and
	      uint8_t(Msg::restart_stream) == 23 and uint8_t(Msg::force_key_frame) == 24 and uint8_t(Msg::recenter) == 29 and
	      uint8_t(Msg::exit) == 71 and uint8_t(Msg::toggle_vr_passthrough) == 74 and
	      uint8_t(Msg::toggle_performance_overlay) == 77 and uint8_t(Msg::toggle_foveated_streaming) == 79 and
	      uint8_t(Msg::disconnect) == 81 and uint8_t(Msg::wake_up_computer) == 90 and uint8_t(Msg::computer_awaken) == 91);
	// VD-FACT(video-tags, enum-values)
	CHECK(tag_frame == 0 and tag_slice == 1 and tag_video_format_old == 2 and tag_subtitle == 3 and tag_video_format == 4);
	CHECK(uint8_t(Source::desktop) == 0 and uint8_t(Source::vr) == 1);
	CHECK(int(VideoCodec::h264) == 1 and int(VideoCodec::hevc) == 2 and int(VideoCodec::h264_plus) == 5 and
	      int(VideoCodec::hevc_10bit) == 6 and int(VideoCodec::av1) == 10 and int(VideoCodec::av1_10bit) == 11);
	CHECK(int(HmdType::quest3) == 1000 and int(HmdType::quest3s) == 1001 and int(Quality::low) == 0 and int(Quality::potato) == -1);
	CHECK(feature_hevc == 0x40000 and feature_av1 == 0x4000 and feature_hand_tracking == 0x20 and feature_eye_tracking == 0x80);
	// VD-FACT(button-bits)
	CHECK(button_a == 1 and button_b == 2 and button_x == 4 and button_y == 8 and button_right_thumb == 0x10 and
	      button_right_trigger == 0x20 and button_right_grip == 0x40 and button_left_thumb == 0x80 and
	      button_left_trigger == 0x100 and button_left_grip == 0x200 and button_enter == 0x400 and button_back == 0x800);
	CHECK(touch_right_thumbrest == 0x40 and touch_left_thumbrest == 0x200 and touch_left_thumb_up == 0x2000);
	// VD-FACT(settings-keys)
	CHECK(std::string(key_spacewarp) == "UseSpacewarp" and std::string(key_annexb) == "UseAnnexB" and
	      std::string(key_framerate) == "VRFramerate" and std::string(key_hmd_type) == "HmdType" and
	      std::string(key_quality) == "VRGraphicsQuality" and std::string(key_ipd) == "IPD" and
	      std::string(key_fov_left) == "FovLeft" and std::string(key_has_vr) == "HasVR" and
	      std::string(key_active_codec) == "ActiveCodec" and std::string(key_streaming_source) == "StreamingSource");
	// VD-FACT(preset-sizes): Quest 3 Low = render 1728x1824, stream 1152x1216; Medium stream 1408x1536
	CHECK(quest3_render[1].width == 1728 and quest3_render[1].height == 1824 and quest3_stream[1].width == 1152 and
	      quest3_stream[1].height == 1216 and quest3_stream[2].width == 1408 and quest3_stream[8].height == 3584);
	// VD-FACT(bitrate), user decision OS = Windows, VD-FACT(version)
	CHECK(max_bitrate_bps == 200'000'000 and std::string(os_windows) == "Windows");
	CHECK(streamer_version[0] == 1 and streamer_version[1] == 34 and streamer_version[2] == 22 and streamer_version[3] == 0);
	// VD-FACT(audio-format)
	CHECK(audio_rate == 48000 and speaker_channels == 2 and mic_channels == 1 and speaker_packet_bytes == 1152);
	// VD-FACT(connect-now, offer-layout, rsa-padding, framedata-prefix)
	CHECK(connect_now_size == 17 and offer_key_chars == 243 and rsa_block_size == 128 and frame_data_prefix == 12);
	CHECK(frame_data_base == 312 and frame_data_hands == 3344 and frame_data_body == 4632 and frame_data_eye == 4700 and
	      frame_data_face == 4992);
}

static void pinned_layouts()
{
	// VD-FACT(eye-delivery): natural alignment, little-endian, as measured on the official structs
	CHECK(sizeof(RenderPose) == 92 and offsetof(RenderPose, eye) == 4 and offsetof(RenderPose, fov) == 60);
	CHECK(offsetof(FrameMetadata, timestamp_us) == 0 and offsetof(FrameMetadata, frame_index) == 8 and
	      offsetof(FrameMetadata, source) == 12 and offsetof(FrameMetadata, is_sync_frame) == 13 and
	      offsetof(FrameMetadata, is_hdr) == 14 and offsetof(FrameMetadata, frame_time_ms) == 16 and
	      offsetof(FrameMetadata, encode_time_ms) == 24 and offsetof(FrameMetadata, pose) == 28 and
	      offsetof(FrameMetadata, haptic) == 120 and offsetof(FrameMetadata, render_width) == 144 and
	      offsetof(FrameMetadata, data_size) == 152 and sizeof(FrameMetadata) == 160);
	CHECK(sizeof(HapticVibration) == 12 and offsetof(HapticVibration, fovea_x) == 4);
	CHECK(sizeof(SliceHeader) == 16 and offsetof(SliceHeader, stream_index) == 8 and offsetof(SliceHeader, data_size) == 12);
	CHECK(sizeof(VideoFormat) == 112 and offsetof(VideoFormat, codec) == 4 and offsetof(VideoFormat, frame_rate) == 8 and
	      offsetof(VideoFormat, frame_size) == 12 and offsetof(VideoFormat, stream_count) == 20 and
	      offsetof(VideoFormat, bounds) == 24 and offsetof(VideoFormat, more) == 40 and video_format_old_size == 88);
	// little-endian raw copy
	FrameMetadata md;
	md.data_size = 0x01020304;
	auto b = bytes_of(md);
	CHECK(b[152] == 4 and b[153] == 3 and b[154] == 2 and b[155] == 1);
	VideoFormat f;
	f.frame_rate = 72;
	f.stream_count = 2;
	auto vf = video_format_message(f);
	CHECK(vf.size() == 89 and vf[0] == 2 and vf[1] == 1 and vf[5] == 2 and vf[9] == 72 and vf[21] == 2);

	// VD-FACT(framedata-layout): tracking sample at fixed offsets
	TrackingSample s;
	s.frame_index = 0x11223344;
	s.latency_s = 0.25f;
	s.input.buttons = button_a | button_left_grip;
	s.input.touches = touch_right_thumbrest;
	s.input.trigger[1] = 200;
	s.input.grip[0] = 7;
	s.input.thumb_x[0] = -5;
	s.input.thumb_y[1] = 32767;
	s.head.position = {1, 2, 3};
	s.head.tracked = true;
	s.controller[1].position = {4, 5, 6};
	s.controller[1].tracked = controller_tracked;
	s.controller[0].battery = 80;
	auto raw = encode_tracking(s);
	CHECK(raw.size() == 312);
	float f32;
	int16_t i16;
	CHECK(raw[0] == 0x44 and raw[3] == 0x11);
	CHECK(raw[94] == 0x01 and raw[95] == 0x02 and raw[96] == 0x40 and raw[99] == 200 and raw[100] == 7);
	memcpy(&i16, &raw[102], 2);
	CHECK(i16 == -5);
	memcpy(&i16, &raw[108], 2);
	CHECK(i16 == 32767);
	memcpy(&f32, &raw[120 + 16], 4);
	CHECK(f32 == 1 and raw[172] == 1);
	memcpy(&f32, &raw[244 + 16 + 8], 4);
	CHECK(f32 == 6 and raw[308] == 1 and raw[176 + 65] == 80);
	auto back = decode_tracking(raw);
	CHECK(back and back->frame_index == s.frame_index and back->latency_s == 0.25f and back->input.buttons == s.input.buttons and
	      back->input.thumb_y[1] == 32767 and back->head.tracked and back->controller[1].position.z == 6 and
	      back->controller[1].tracked == controller_tracked and back->size == 312);
	auto big = encode_tracking(s, frame_data_eye);
	CHECK(decode_tracking(big)->size == frame_data_eye);
	CHECK(not decode_tracking(std::span(raw.data(), 311)));
}

static void big_endian_framing()
{
	uint8_t payload[] = {0xaa, 0xbb};
	auto m = control_message(Msg::force_key_frame, payload);
	CHECK((m == std::vector<uint8_t>{24, 0, 0, 0, 2, 0xaa, 0xbb}));
	BeWriter w;
	w.u16(0x0102);
	w.u32(0x03040506);
	w.u64(0x0708090a0b0c0d0eull);
	w.str("hé");
	uint8_t a4[] = {192, 168, 1, 2};
	w.address(a4, 38810);
	CHECK(w.out.size() == 2 + 4 + 8 + 2 + 3 + 1 + 4 + 4);
	CHECK(w.out[0] == 1 and w.out[1] == 2 and w.out[14] == 0 and w.out[15] == 3);
	BeReader r(w.out);
	CHECK(r.u16() == 0x0102 and r.u32() == 0x03040506 and r.u64() == 0x0708090a0b0c0d0eull and r.str() == "hé");
	uint32_t port = 0;
	CHECK((r.address(port) == std::vector<uint8_t>{192, 168, 1, 2}) and port == 38810 and r.left() == 0);
	// VD-FACT(settings-payload): a change is a one-member JSON object
	CHECK(setting_changed_payload("UseSpacewarp", "true") == R"({"UseSpacewarp":true})");
	auto sc = control_message(Msg::mobile_setting_changed, setting_changed_payload("A", "1"));
	CHECK(sc[0] == 14 and sc[4] == 7 and std::string(sc.begin() + 5, sc.end()) == R"({"A":1})");

	std::vector<uint8_t> v;
	put_varint(v, 300);
	CHECK((v == std::vector<uint8_t>{0xac, 0x02}));
	size_t used = 0;
	CHECK(get_varint(v, used) == 300u and used == 2);
	uint8_t cut[] = {0x80};
	CHECK(not get_varint(cut, used));
}

static void stream_wrapper()
{
	AesKey k = AesKey::random();
	for (bool enc: {false, true})
	{
		int sv[2];
		REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		Channel a(sv[0]);
		int raw = sv[1];
		if (enc)
			a.encrypt(k);
		std::vector<uint8_t> msg(37, 0x5a);
		CHECK(a.send(msg) and a.send(msg));
		std::vector<uint8_t> wire(256);
		ssize_t n = recv(raw, wire.data(), enc ? 96 : 74, MSG_WAITALL);
		CHECK(recv(raw, wire.data() + n, 16, MSG_DONTWAIT) < 0); // nothing more
		if (not enc)
			CHECK(n == 74 and std::equal(msg.begin(), msg.end(), wire.begin())); // VD-FACT(plain-framing)
		else
		{
			// varint(37) + 37 = 38 -> 48 bytes per chunk; continuous CBC: equal messages, different ciphertext
			CHECK(n == 96);
			CHECK(not std::equal(wire.begin(), wire.begin() + 48, wire.begin() + 48));
			CbcStream d(k, false);
			auto plain = d.update(std::span(wire.data(), 96));
			CHECK(plain[0] == 37 and plain[1] == 0x5a and plain[48] == 37 and plain[49] == 0x5a);
			close(raw);
			// a peer Channel reads what a Channel writes, across chunk boundaries (fresh chains on both sides)
			int sp[2];
			REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
			Channel w(sp[0]), b(sp[1]);
			w.encrypt(k);
			b.encrypt(k);
			std::vector<uint8_t> big(5000);
			for (size_t i = 0; i < big.size(); ++i)
				big[i] = uint8_t(i * 7);
			std::thread t([&] { w.send(big); w.send(msg); });
			std::vector<uint8_t> got(5000 + 37);
			CHECK(b.read_exact(got.data(), 100, 2000) and b.read_exact(got.data() + 100, got.size() - 100, 2000));
			t.join();
			CHECK(std::equal(big.begin(), big.end(), got.begin()) and got.back() == 0x5a);
			continue;
		}
		close(raw);
	}
}

static void discovery()
{
	AesKey disc = AesKey::random();
	RsaKeyPair rsa(1024);
	CHECK(rsa.public_xml().size() == offer_key_chars and rsa.size() == rsa_block_size);
	// VD-FACT(offer-layout)
	Offer o{.rsa_public_xml = rsa.public_xml(), .platform = 1, .account = "acc"};
	auto plain = encode_offer(o);
	CHECK(plain.size() == 243 + 2 + 3 and plain[243] == 0 and plain[244] == 1 and plain.back() == 'c');
	auto p = parse_offer(plain);
	CHECK(p and p->rsa_public_xml == o.rsa_public_xml and p->platform == 1 and p->account == "acc");
	auto bare = parse_offer(encode_offer({.rsa_public_xml = o.rsa_public_xml, .account = "x"}));
	CHECK(bare and not bare->platform and bare->account == "x");
	CHECK(not parse_offer(std::vector<uint8_t>(250, 'x')));
	// VD-FACT(discovery-cipher): chunk format, no PKCS#7; several chunks concatenate
	auto wrapped = wrap_offer(disc, o);
	CHECK(wrapped.size() == 256); // varint(248) = 2 bytes + 248 -> 250 -> 256
	CHECK(unwrap_offer(disc, wrapped)->account == "acc");
	CHECK(not unwrap_offer(AesKey::random(), wrapped));
	std::vector<uint8_t> parts[] = {std::vector<uint8_t>(plain.begin(), plain.begin() + 243),
	                                std::vector<uint8_t>(plain.begin() + 243, plain.end())};
	auto two = chunk_wrap(disc, parts);
	CHECK(two.size() == 256 + 16 and chunk_unwrap(disc, two) == plain and unwrap_offer(disc, two)->platform == 1);
	CHECK(not chunk_unwrap(disc, std::span(two.data(), 20)));

	// VD-FACT(connect-now): plain
	ConnectNow c;
	c.connection_id[0] = 9;
	c.platform = 3;
	auto cn = encode_connect_now(c);
	CHECK(cn.size() == 17 and cn[0] == 9 and cn[16] == 3 and parse_connect_now(cn)->connection_id[0] == 9);

	// VD-FACT(record-schema): the exact DataContract form (golden from the stock .NET serializer on our own mirror)
	ComputerRecord rec;
	rec.id = "id-1";
	rec.name = "Mac";
	rec.description = "desc";
	rec.adapters = {{.ip = "192.168.1.10", .mac = "AA-BB-CC-DD-EE-FF", .wireless = true}};
	rec.channel_key.key.fill(0);
	rec.channel_key.iv.fill(0);
	CHECK(encode_record(rec) ==
	      R"(<Computer xmlns="http://schemas.datacontract.org/2004/07/VirtualDesktop.Interfaces" xmlns:i="http://www.w3.org/2001/XMLSchema-instance">)"
	      R"(<AllowRemoteConnections>false</AllowRemoteConnections><ConnectionID>AAAAAAAAAAAAAAAAAAAAAA==</ConnectionID>)"
	      R"(<Description>desc</Description><EncryptLocalTraffic>true</EncryptLocalTraffic><EncryptRemoteTraffic>true</EncryptRemoteTraffic>)"
	      R"(<ID>id-1</ID><IV>AAAAAAAAAAAAAAAAAAAAAA==</IV><Key>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</Key><Name>Mac</Name>)"
	      R"(<OS>Windows</OS><PrivateAdapters><NetworkAdapter><IPAddresString>192.168.1.10</IPAddresString><IsGigabit>false</IsGigabit>)"
	      R"(<IsUsb>false</IsUsb><IsWireless>true</IsWireless><MACAddresString>AA-BB-CC-DD-EE-FF</MACAddresString></NetworkAdapter>)"
	      R"(</PrivateAdapters><Region>Europe</Region><StreamerVersion xmlns:a="http://schemas.datacontract.org/2004/07/System">)"
	      R"(<a:_Build>22</a:_Build><a:_Major>1</a:_Major><a:_Minor>34</a:_Minor><a:_Revision>0</a:_Revision></StreamerVersion></Computer>)");
	rec.name = "Mac <&>";
	rec.channel_key = AesKey::random();
	random_bytes(rec.connection_id);
	auto back = parse_record(encode_record(rec));
	CHECK(back and back->name == "Mac <&>" and back->adapters.size() == 1 and back->adapters[0].wireless and
	      back->channel_key.key == rec.channel_key.key and back->connection_id == rec.connection_id and
	      back->version[1] == 34 and back->os == "Windows");

	// VD-FACT(reply-layout, rsa-padding): OAEP block of a wrapper key, then the record under that key
	auto reply = encode_reply(rsa.public_xml(), rec);
	REQUIRE(reply);
	CHECK(reply->size() > 128 and (reply->size() - 128) % 16 == 0);
	auto r = parse_reply(*reply, [&](std::span<const uint8_t> b) { return rsa.decrypt(b); });
	CHECK(r and r->channel_key.iv == rec.channel_key.iv and r->name == rec.name);
	RsaKeyPair big(2048);
	CHECK(not encode_reply(big.public_xml(), rec)); // the Streamer copies exactly 128 bytes: 1024-bit keys only
	CHECK(not rsa_encrypt("<RSAKeyValue>junk</RSAKeyValue>", plain));
	CHECK(base64(std::vector<uint8_t>{1, 2, 3, 4}) == "AQIDBA==" and unbase64("AQIDBA==") == std::vector<uint8_t>({1, 2, 3, 4}));
}

static void settings()
{
	Settings s;
	CHECK(s.parse(R"( { "A" : 1.5, "B":"x\"y", "C": {"n":[1,{"z":"}"}]}, "D": true, "E": null } )"));
	CHECK(s.number("A") == 1.5 and s.string("B") == "x\"y" and s.boolean("D") == true and s.raw("E") == "null");
	CHECK(s.raw("C") == R"({"n":[1,{"z":"}"}]})");
	CHECK(not s.number("B") and not s.boolean("A"));
	s.set_raw("A", "2");
	s.set("F", std::string("q"));
	Settings t;
	CHECK(t.parse(s.to_json()) and t.number("A") == 2 and t.string("F") == "q" and t.raw("C") == s.raw("C"));
	CHECK(not t.parse("[1]") and not t.parse("{\"a\":}") and t.number("A") == 2);
	CHECK(t.parse("{}") and t.empty());
	Settings change;
	CHECK(change.parse(R"({"UseSpacewarp":true})"));
	t.merge(change);
	CHECK(t.boolean("UseSpacewarp") == true);
}

// A1: render divisor 2 -> wake-ups two display periods apart, display period stays the panel's.
static void pacing()
{
	for (auto mode: {VdConfig::Spacewarp::off, VdConfig::Spacewarp::on})
	{
		VdSession s({.spacewarp = mode});
		uint32_t d = mode == VdConfig::Spacewarp::on ? 2 : 1;
		CHECK(s.render_divisor() == d);
		std::vector<FrameTiming> t;
		for (int i = 0; i < 12; ++i)
			t.push_back(s.next_frame_timing());
		double period = 1e9 / 72; // VD-FACT(ssw-cadence): Quest-class default 72 Hz
		CHECK(std::abs(t[0].display_period - period) < 1000 and t[5].render_divisor == d);
		double mean = double(t.back().wake_up - t[1].wake_up) / (t.size() - 2);
		printf("pacing: divisor %u, mean frame interval %.2f ms (display period %.2f ms)\n", d, mean / 1e6, period / 1e6);
		CHECK(std::abs(mean - d * period) < 0.05 * d * period); // the grid itself, not sleep accuracy
		CHECK(t[3].frame_id == t[2].frame_id + 1 and t[3].predicted_display > t[3].wake_up);
		CHECK(now_ns() - t.back().wake_up < ns(d * period) + 5'000'000);
	}
}

int main()
{
	pinned_constants();
	pinned_layouts();
	big_endian_framing();
	stream_wrapper();
	discovery();
	settings();
	pacing();
	return test_result("vd_wire");
}
