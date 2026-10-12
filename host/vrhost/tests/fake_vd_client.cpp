// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_vd_client.h"

#include "session/clock.h"
#include "vd/vd_channel.h"
#include "vd/vd_crypto.h"
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
struct Discovery
{
	RsaKeyPair rsa{2048};
	std::optional<Reply> reply;
};

void discover(const FakeVdOptions & o, int timeout_ms, Discovery & d)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(o.discovery_port);
	inet_pton(AF_INET, o.host.c_str(), &to.sin_addr);
	auto offer = encode_offer({.rsa_public_xml = d.rsa.public_xml(), .platform = o.platform, .account = o.account});
	auto wrapped = aes_wrap(o.discovery_key, offer);
	sendto(fd, wrapped.data(), wrapped.size(), 0, (sockaddr *)&to, sizeof to);
	pollfd p{fd, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) == 1)
	{
		std::vector<uint8_t> buf(65536);
		ssize_t n = recv(fd, buf.data(), buf.size(), 0);
		if (n > 0)
			d.reply = parse_reply(o.discovery_key, std::span(buf.data(), size_t(n)), d.rsa.size(),
			                      [&](std::span<const uint8_t> b) { return d.rsa.decrypt(b); });
	}
	close(fd);
}

std::vector<uint8_t> str_bytes(const std::string & s)
{
	return {s.begin(), s.end()};
}

std::vector<uint8_t> frame_data_packet(const FrameData & fd)
{
	auto raw = bytes_of(fd);
	std::vector<uint8_t> comp(LZ4_compressBound(int(raw.size())));
	int n = LZ4_compress_default((const char *)raw.data(), (char *)comp.data(), int(raw.size()), int(comp.size()));
	comp.resize(size_t(n));
	int32_t sizes[2] = {n, int32_t(raw.size())};
	uint64_t hash = XXH3_64bits(raw.data(), raw.size());
	std::vector<uint8_t> out(16);
	memcpy(out.data(), sizes, 8);
	memcpy(out.data() + 8, &hash, 8);
	out.insert(out.end(), comp.begin(), comp.end());
	return out;
}
} // namespace

std::optional<Reply> fake_vd_discover(const FakeVdOptions & o, int timeout_ms)
{
	Discovery d;
	discover(o, timeout_ms, d);
	return d.reply;
}

FakeVdResult run_fake_vd_client(const FakeVdOptions & o)
{
	FakeVdResult r;
	Discovery disc;
	discover(o, 3000, disc);
	if (not disc.reply)
	{
		r.error = "no discovery reply";
		return r;
	}
	const Reply & reply = *disc.reply;
	r.record = reply.record;
	if (reply.record.addresses.empty())
	{
		r.error = "record without address";
		return r;
	}
	std::string addr = reply.record.addresses.front();
	auto open = [&](uint16_t port) -> std::unique_ptr<Channel> {
		int fd = connect_tcp(addr, port);
		return fd < 0 ? nullptr : std::make_unique<Channel>(fd);
	};

	// control: PeerInfo both ways, then (maybe) encryption, then our settings
	auto ctl = open(reply.record.control_port);
	if (not ctl)
	{
		r.error = "control connect failed";
		return r;
	}
	ctl->send(encode_peer_info({.address = {127, 0, 0, 1}, .port = 0, .encrypt = true}));
	uint8_t family = 0;
	std::vector<uint8_t> rest;
	if (not ctl->read_exact(&family, 1, 5000) or (family != 4 and family != 16) or
	    (rest.resize(family + 5), not ctl->read_exact(rest.data(), rest.size(), 5000)))
	{
		r.error = "no host PeerInfo";
		return r;
	}
	r.peer_encrypt = rest.back() != 0;
	if (r.peer_encrypt)
		ctl->encrypt(reply.session);
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
			if (Msg(hdr[0]) == Msg::reload_streamer_settings)
				r.streamer_settings.assign(payload.begin(), payload.end());
			else if (Msg(hdr[0]) == Msg::streamer_setting_changed)
			{
				BeReader br(payload);
				auto name = br.str();
				r.streamer_changes.push_back({name, br.str()});
			}
		}
	});
	ctl->send(control_message(Msg::reload_user_settings, str_bytes("{}")));
	ctl->send(control_message(Msg::reload_mobile_settings, str_bytes(o.mobile_settings)));

	auto data = open(reply.record.data_port);
	auto video = open(reply.record.video_port);
	auto audio = o.audio ? open(reply.record.audio_port) : nullptr;
	std::thread audio_reader;
	if (not data or not video or (o.audio and not audio))
		r.error = "channel connect failed";
	else
	{
		if (reply.record.encrypt_local)
			for (Channel * c: {data.get(), video.get(), audio.get()})
				if (c)
					c->encrypt(reply.session);
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
			else if (o.annexb or bits.empty() or not annexb::append_length_prefixed(annexb, bits, 4))
			{
				r.length_prefix_errors++;
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
			if (tag == tag_video_format)
			{
				VideoFormat f;
				if (not video->read_exact(&f, sizeof f, 5000))
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
				if (o.frame_data)
				{
					FrameData fd = *o.frame_data;
					fd.frame_index = complete;
					data->send(frame_data_packet(fd));
				}
				if (complete == o.force_key_frame_after)
					ctl->send(control_message(Msg::force_key_frame, {}));
				if (complete == 1)
					for (auto & [k, v]: o.mobile_changes)
						ctl->send(control_message(Msg::mobile_setting_changed, setting_changed_payload(k, v)));
			}
			else
			{
				r.error = "unknown video tag " + std::to_string(tag);
				break;
			}
		}
		if (r.error.empty() and complete < o.frames)
			r.error = "timeout after " + std::to_string(complete) + " frames";
		ctl->send(control_message(Msg::disconnect, {}));
	}
	for (Channel * c: {ctl.get(), data.get(), video.get(), audio.get()})
		if (c)
			c->shutdown();
	control_reader.join();
	if (audio_reader.joinable())
		audio_reader.join();
	return r;
}
