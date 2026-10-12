// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_vd_client.h"

#include "session/clock.h"
#include "vd/vd_channel.h"
#include "vd/vd_crypto.h"
#include "vd/vd_settings.h"
#include "vt_decode.h"
#include "wivrn/annexb.h"

#include <lz4.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <arpa/inet.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace vrhost;
using namespace vrhost::vd;

namespace
{
void send_datagram(const FakeVdOptions & o, std::span<const uint8_t> d, int fd)
{
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(o.discovery_port);
	inet_pton(AF_INET, o.host.c_str(), &to.sin_addr);
	sendto(fd, d.data(), d.size(), 0, (sockaddr *)&to, sizeof to);
}

std::optional<ComputerRecord> discover(const FakeVdOptions & o, int timeout_ms)
{
	RsaKeyPair rsa(1024);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	auto offer = wrap_offer(o.discovery_key, {.rsa_public_xml = rsa.public_xml(), .platform = o.platform, .account = o.account});
	send_datagram(o, offer, fd);
	std::optional<ComputerRecord> r;
	pollfd p{fd, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) == 1)
	{
		std::vector<uint8_t> buf(65536);
		ssize_t n = recv(fd, buf.data(), buf.size(), 0);
		if (n > 0)
			r = parse_reply(std::span(buf.data(), size_t(n)), [&](std::span<const uint8_t> b) { return rsa.decrypt(b); });
	}
	close(fd);
	return r;
}

std::vector<uint8_t> tracking_packet(const TrackingSample & s, size_t size)
{
	auto raw = encode_tracking(s, size);
	std::vector<uint8_t> comp(LZ4_compressBound(int(raw.size())));
	int n = LZ4_compress_default((const char *)raw.data(), (char *)comp.data(), int(raw.size()), int(comp.size()));
	comp.resize(size_t(n));
	uint64_t hash = XXH3_64bits(raw.data(), raw.size());
	std::vector<uint8_t> out(frame_data_prefix);
	memcpy(out.data(), &n, 4);
	memcpy(out.data() + 4, &hash, 8);
	out.insert(out.end(), comp.begin(), comp.end());
	return out;
}
} // namespace

std::optional<ComputerRecord> fake_vd_discover(const FakeVdOptions & o, int timeout_ms)
{
	return discover(o, timeout_ms);
}

void fake_vd_connect_now(const FakeVdOptions & o, const ComputerRecord & r)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	send_datagram(o, encode_connect_now({.connection_id = r.connection_id, .platform = o.platform}), fd);
	close(fd);
}

FakeVdResult run_fake_vd_client(const FakeVdOptions & o, const std::function<void()> & after_first_frame)
{
	FakeVdResult r;
	r.record = discover(o, 3000);
	if (not r.record)
	{
		r.error = "no discovery reply";
		return r;
	}
	const ComputerRecord & rec = *r.record;
	if (rec.adapters.empty())
	{
		r.error = "record without address";
		return r;
	}
	fake_vd_connect_now(o, rec);
	std::string addr = rec.adapters.front().ip;
	auto open = [&](uint16_t port) -> std::unique_ptr<Channel> {
		int fd = connect_tcp(addr, port);
		return fd < 0 ? nullptr : std::make_unique<Channel>(fd);
	};

	// control: always encrypted on the LAN
	auto ctl = open(o.control_port);
	if (not ctl)
	{
		r.error = "control connect failed";
		return r;
	}
	ctl->encrypt(rec.channel_key);
	std::mutex rm;
	std::thread control_reader([&] {
		uint8_t hdr[5];
		while (ctl->read_exact(hdr, 5))
		{
			uint32_t n = uint32_t(hdr[1]) << 24 | uint32_t(hdr[2]) << 16 | uint32_t(hdr[3]) << 8 | hdr[4];
			std::vector<uint8_t> payload(n);
			if (n and not ctl->read_exact(payload.data(), n))
				break;
			std::lock_guard lock(rm);
			r.control_types.push_back(hdr[0]);
			std::string text(payload.begin(), payload.end());
			if (Msg(hdr[0]) == Msg::reload_streamer_settings)
				r.streamer_settings = text;
			else if (Msg(hdr[0]) == Msg::streamer_setting_changed)
			{
				Settings s;
				if (s.parse(text))
					for (auto & k: {key_active_codec, key_allow_extrapolation})
						if (auto v = s.raw(k))
							r.streamer_changes.push_back({k, *v});
			}
		}
	});
	ctl->send(control_message(Msg::reload_user_settings, o.user_settings));
	ctl->send(control_message(Msg::reload_mobile_settings, o.mobile_settings));

	// the Streamer's order: video, audio, data
	auto video = open(o.video_port);
	auto audio = o.audio ? open(o.audio_port) : nullptr;
	auto data = open(o.data_port);
	std::thread audio_reader;
	if (not data or not video or (o.audio and not audio))
		r.error = "channel connect failed";
	else
	{
		if (rec.encrypt_local)
			for (Channel * c: {data.get(), video.get(), audio.get()})
				if (c)
					c->encrypt(rec.channel_key);
		uint8_t probe[4];
		if (data->read_exact(probe, 4, 5000))
		{
			int32_t n = int32_t(uint32_t(probe[0]) << 24 | uint32_t(probe[1]) << 16 | uint32_t(probe[2]) << 8 | probe[3]);
			r.bandwidth_probe = n;
			std::vector<uint8_t> junk(size_t(std::max(n, 0)));
			if (n > 0)
				data->read_exact(junk.data(), junk.size(), 5000);
		}
		if (audio)
			audio_reader = std::thread([&] {
				int32_t n;
				while (audio->read_exact(&n, 4))
				{
					std::vector<int16_t> pcm(size_t(n) / 2);
					if (n and not audio->read_exact(pcm.data(), size_t(n)))
						break;
					std::lock_guard lock(rm);
					r.speaker.insert(r.speaker.end(), pcm.begin(), pcm.end());
					r.speaker_packets++;
					r.speaker_max_packet = std::max(r.speaker_max_packet, n);
				}
			});

		std::array<std::unique_ptr<VtDecoder>, 2> dec;
		bool mic_sent = false;
		int complete = 0;
		int64_t deadline = now_ns() + int64_t(o.timeout_ms) * 1000000;
		auto decode = [&](int eye, std::span<const uint8_t> bits) {
			std::vector<uint8_t> annexb;
			bool start_code = bits.size() > 4 and bits[0] == 0 and bits[1] == 0 and bits[2] == 0 and bits[3] == 1;
			if (o.annexb and start_code)
				annexb.assign(bits.begin(), bits.end());
			else if (o.annexb or bits.empty() or start_code or not annexb::append_length_prefixed(annexb, bits, 4))
			{
				r.framing_errors++;
				return;
			}
			if (not o.decode or r.formats.empty())
				return;
			if (not dec[eye])
				dec[eye] = std::make_unique<VtDecoder>(r.formats.back().codec == VideoCodec::hevc);
			for (auto nal: annexb::split(annexb))
				if (annexb::hevc_nal_type(nal) == annexb::hevc_vps)
					r.idr[eye]++;
			if (auto pb = dec[eye]->decode(annexb))
			{
				r.decoded[eye]++;
				CVPixelBufferRelease(pb);
			}
		};
		while (complete < o.frames and now_ns() < deadline)
		{
			uint8_t tag;
			if (not video->read_exact(&tag, 1, 5000))
			{
				r.error = "video channel: nothing within 5 s";
				break;
			}
			if (tag == tag_video_format_old)
			{
				VideoFormat f;
				if (not video->read_exact(&f, video_format_old_size, 5000))
					break;
				r.formats.push_back(f);
				if (not mic_sent and not o.microphone.empty() and audio)
				{
					std::vector<uint8_t> m(4 + o.microphone.size() * 2);
					int32_t n = int32_t(o.microphone.size() * 2);
					memcpy(m.data(), &n, 4);
					memcpy(m.data() + 4, o.microphone.data(), size_t(n));
					audio->send(m);
					mic_sent = true;
				}
			}
			else if (tag == tag_frame)
			{
				FrameMetadata md;
				if (not video->read_exact(&md, sizeof md, 5000))
					break;
				std::vector<uint8_t> bits(size_t(md.data_size));
				if (not video->read_exact(bits.data(), bits.size(), 5000))
					break;
				r.frames.push_back(md);
				decode(0, bits);
			}
			else if (tag == tag_slice)
			{
				SliceHeader sh;
				if (not video->read_exact(&sh, sizeof sh, 5000))
					break;
				std::vector<uint8_t> bits(size_t(sh.data_size));
				if (not video->read_exact(bits.data(), bits.size(), 5000))
					break;
				r.slices.push_back(sh);
				if (sh.stream_index == 1)
					decode(1, bits);
				++complete;
				if (o.tracking)
				{
					TrackingSample s = *o.tracking;
					s.frame_index = 1000 + complete;
					data->send(tracking_packet(s, o.tracking_size));
				}
				if (complete == o.force_key_frame_after)
					ctl->send(control_message(Msg::force_key_frame, std::string()));
				if (complete == 1)
				{
					for (auto & [k, v]: o.user_changes)
						ctl->send(control_message(Msg::user_setting_changed, setting_changed_payload(k, v)));
					for (auto & [k, v]: o.mobile_changes)
						ctl->send(control_message(Msg::mobile_setting_changed, setting_changed_payload(k, v)));
					if (o.extra_requests)
					{
						ctl->send(control_message(Msg::update_required, std::string()));
						ctl->send(control_message(Msg::wake_up_computer, std::string()));
						ctl->send(control_message(Msg::recenter, std::string()));
					}
					if (after_first_frame)
						after_first_frame();
				}
			}
			else
			{
				r.error = "unknown video tag " + std::to_string(tag);
				break;
			}
		}
		if (r.error.empty() and complete < o.frames)
			r.error = "timeout after " + std::to_string(complete) + " frames";
		std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let late control replies arrive
		ctl->send(control_message(Msg::disconnect, std::string()));
	}
	for (Channel * c: {ctl.get(), data.get(), video.get(), audio.get()})
		if (c)
			c->shutdown();
	control_reader.join();
	if (audio_reader.joinable())
		audio_reader.join();
	return r;
}
