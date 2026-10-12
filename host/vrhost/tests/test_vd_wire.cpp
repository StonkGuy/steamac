// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter, offline: pins every assumption of src/vd/vd_assumptions.h (a change there must change this
// test), and checks the wire primitives: big-endian control framing, varints, the AES stream wrapper on a socket pair,
// discovery datagrams and the computer record, settings objects, and the render-divisor pacing (A1).
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
	// ports (notes + capture)
	CHECK(default_control_port == 38810 and default_data_port == 38820 and default_video_port == 38830 and
	      default_audio_port == 38840 and default_discovery_port == 38850);
	VdConfig cfg;
	CHECK(cfg.control_port == 38810 and cfg.data_port == 38820 and cfg.video_port == 38830 and cfg.audio_port == 38840 and
	      cfg.discovery_port == 38850);
	// TODO(vd-live:control-ids)
	CHECK(uint8_t(Msg::reload_user_settings) == 1 and uint8_t(Msg::reload_mobile_settings) == 2 and
	      uint8_t(Msg::reload_streamer_settings) == 3 and uint8_t(Msg::user_setting_changed) == 4 and
	      uint8_t(Msg::mobile_setting_changed) == 5 and uint8_t(Msg::streamer_setting_changed) == 6 and
	      uint8_t(Msg::force_key_frame) == 10 and uint8_t(Msg::disconnect) == 20 and uint8_t(Msg::exit) == 21);
	// TODO(vd-live:video-tags), TODO(vd-live:enum-values)
	CHECK(tag_frame == 0 and tag_slice == 1 and tag_video_format == 2);
	CHECK(int(Source::vr) == 1 and int(VideoCodec::h264) == 0 and int(VideoCodec::hevc) == 2);
	// TODO(vd-live:button-bits)
	CHECK(button_a == 1 and button_b == 2 and button_x == 4 and button_y == 8 and button_thumb == 16 and
	      button_trigger == 32 and button_grip == 64 and button_thumbrest == 128 and button_enter == 1024 and button_back == 2048);
	// TODO(vd-live:settings-keys)
	CHECK(std::string(key_spacewarp) == "UseSpacewarp" and std::string(key_annexb) == "UseAnnexB" and
	      std::string(key_framerate) == "Framerate" and std::string(key_hmd_type) == "HmdType");
	// user facts: Quest 3 "Low" floor, 200 Mbit/s cap; user decision: OS = Windows
	CHECK(quest3_low_eye == 1700 and max_bitrate_bps == 200'000'000 and std::string(os_windows) == "Windows");
	// TODO(vd-live:audio-format)
	CHECK(audio_rate == 48000 and speaker_channels == 2 and mic_channels == 1);
	CHECK(connect_now_size == 17);
}

static void pinned_layouts()
{
	// TODO(vd-live:*-layout): natural alignment, little-endian
	CHECK(sizeof(RenderPose) == 92 and offsetof(RenderPose, eye) == 4 and offsetof(RenderPose, fov) == 60);
	CHECK(offsetof(FrameMetadata, timestamp_us) == 0 and offsetof(FrameMetadata, frame_index) == 8 and
	      offsetof(FrameMetadata, source) == 16 and offsetof(FrameMetadata, is_sync_frame) == 20 and
	      offsetof(FrameMetadata, is_hdr) == 21 and offsetof(FrameMetadata, frame_time_ms) == 24 and
	      offsetof(FrameMetadata, pose) == 36 and offsetof(FrameMetadata, haptic) == 128 and
	      offsetof(FrameMetadata, render_width) == 144 and offsetof(FrameMetadata, data_size) == 152 and
	      sizeof(FrameMetadata) == 160);
	CHECK(sizeof(SliceHeader) == 16 and offsetof(SliceHeader, stream_index) == 8);
	CHECK(sizeof(VideoFormat) == 120 and offsetof(VideoFormat, frame_rate) == 8 and offsetof(VideoFormat, stream_count) == 20 and
	      offsetof(VideoFormat, stream) == 24);
	CHECK(sizeof(ControllerInput) == 16 and offsetof(ControllerInput, trigger) == 8 and offsetof(ControllerInput, thumb_x) == 10);
	CHECK(sizeof(RawInputState) == 108 and offsetof(RawInputState, keyboard) == 48);
	CHECK(sizeof(TrackedDevice) == 60 and offsetof(TrackedDevice, orientation_tracked) == 52 and offsetof(TrackedDevice, battery) == 56);
	CHECK(sizeof(FrameData) == 312 and offsetof(FrameData, input) == 16 and offsetof(FrameData, lock_states) == 124 and
	      offsetof(FrameData, head) == 128 and offsetof(FrameData, controller) == 188);
	// little-endian raw copy
	FrameMetadata md;
	md.data_size = 0x01020304;
	auto b = bytes_of(md);
	CHECK(b[152] == 4 and b[153] == 3 and b[154] == 2 and b[155] == 1);
}

static void big_endian_framing()
{
	uint8_t payload[] = {0xaa, 0xbb};
	auto m = control_message(Msg::force_key_frame, payload);
	CHECK((m == std::vector<uint8_t>{10, 0, 0, 0, 2, 0xaa, 0xbb}));
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
	auto pi = encode_peer_info({.address = {10, 0, 0, 1}, .port = 7, .encrypt = true});
	CHECK((pi == std::vector<uint8_t>{4, 10, 0, 0, 1, 0, 0, 0, 7, 1}));
	auto sc = setting_changed_payload("A", "1");
	CHECK((sc == std::vector<uint8_t>{0, 1, 'A', 0, 1, '1'}));

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
			CHECK(n == 74 and std::equal(msg.begin(), msg.end(), wire.begin())); // TODO(vd-live:plain-framing)
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
	AesKey disc = AesKey::random(), sess = AesKey::random();
	RsaKeyPair rsa(2048);
	Offer o{.rsa_public_xml = rsa.public_xml(), .platform = 0x0102, .account = "acc"};
	auto plain = encode_offer(o);
	auto end = o.rsa_public_xml.size();
	CHECK(plain[end] == 1 and plain[end + 1] == 2 and plain.back() == 'c'); // TODO(vd-live:offer-layout)
	auto p = parse_offer(plain);
	CHECK(p and p->rsa_public_xml == o.rsa_public_xml and p->platform == 0x0102 and p->account == "acc");
	auto bare = parse_offer(std::span(plain.data(), end));
	CHECK(bare and not bare->platform and bare->account.empty());
	CHECK(not parse_offer(std::vector<uint8_t>{'x'}));

	auto wrapped = aes_wrap(disc, plain);
	CHECK(wrapped.size() % 16 == 0 and wrapped.size() > plain.size()); // TODO(vd-live:discovery-cipher): PKCS#7
	CHECK(aes_unwrap(disc, wrapped) == plain);
	CHECK(aes_unwrap(AesKey::random(), wrapped) != plain);

	ConnectNow c;
	c.guid[0] = 9;
	c.platform = 3;
	auto cn = encode_connect_now(c);
	CHECK(cn.size() == 17 and parse_connect_now(cn)->guid[0] == 9 and parse_connect_now(cn)->platform == 3);

	ComputerRecord rec;
	rec.id = "id";
	rec.name = "Mac <&>";
	rec.addresses = {"192.168.1.5", "10.0.0.2"};
	rec.control_port = 1;
	rec.audio_port = 4;
	auto x = encode_record(rec);
	CHECK(x.find("<OS>Windows</OS>") != std::string::npos and x.find("<Version>1.34.22</Version>") != std::string::npos);
	auto back = parse_record(x);
	CHECK(back and back->name == "Mac <&>" and back->addresses.size() == 2 and back->audio_port == 4 and back->encrypt_local);

	auto reply = encode_reply(disc, sess, rsa.public_xml(), rec);
	REQUIRE(reply);
	CHECK(reply->size() > rsa.size()); // TODO(vd-live:reply-layout): RSA block first
	auto r = parse_reply(disc, *reply, rsa.size(), [&](std::span<const uint8_t> b) { return rsa.decrypt(b); });
	CHECK(r and r->session.key == sess.key and r->session.iv == sess.iv and r->record.addresses == rec.addresses);
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
		double period = 1e9 / 90;
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
