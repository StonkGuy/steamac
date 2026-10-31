// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_headset.h"
#include "pattern/test_pattern.h"
#include "session/clock.h"
#include "vt_decode.h"
#include "wivrn/annexb.h"

#include "crypto.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <random>
#include <cmath>
#include <magic_enum.hpp>
#include <map>
#include <poll.h>

namespace fh = wivrn::from_headset;
namespace th = wivrn::to_headset;
using control_socket = wivrn::typed_socket<wivrn::TCP, th::packets, fh::packets>;
using stream_socket = wivrn::typed_socket<wivrn::UDP, th::packets, fh::packets>;

namespace
{
struct pending_frame
{
	std::map<uint16_t, std::vector<uint8_t>> shards;
	int last = -1;
	std::optional<th::video_stream_data_shard::view_info_t> view_info;
};

XrPosef yaw_pose(float a)
{
	return XrPosef{.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)}, .position = {0, 1.6f, 0}};
}
} // namespace

FakeHeadsetResult run_fake_headset(const FakeHeadsetOptions & o)
{
	FakeHeadsetResult res;
	auto hs_now = [&] { return vrhost::now_ns() + o.clock_offset_ns; };
	try
	{
		sockaddr_in6 addr{};
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons(o.port);
		sockaddr_in addr4{};
		addr4.sin_family = AF_INET;
		addr4.sin_port = htons(o.port);
		bool ipv4 = inet_pton(AF_INET, o.host.c_str(), &addr4.sin_addr) == 1;
		if (not ipv4 and inet_pton(AF_INET6, o.host.c_str(), &addr.sin6_addr) != 1)
			throw std::runtime_error("bad host address " + o.host);
		control_socket control = ipv4 ? control_socket(addr4) : control_socket(addr);

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.timeout_ms);
		bool from_control = false; // the last packet receive() returned came over TCP
		// Network shaping of video shards (FakeHeadsetOptions::Network): queued with their release time
		using clk = std::chrono::steady_clock;
		std::deque<std::pair<clk::time_point, th::packets>> delayed;
		clk::time_point last_release{}, rate_free{}, net_start = clk::now();
		std::mt19937 rng(12345);
		std::uniform_real_distribution<float> uni(0, 1);
		const bool shaping = o.net.delay_ms > 0 or o.net.jitter_ms > 0 or o.net.loss_pct > 0 or o.net.mbps > 0 or o.net.burst_ms > 0;
		// a stream packet through the shaper: false = dropped (or queued for later)
		auto shape = [&](th::packets && p) -> bool {
			auto * shard = std::get_if<th::video_stream_data_shard>(&p);
			if (not shaping or not shard)
				return true;
			auto now = clk::now();
			double since = std::chrono::duration<double>(now - net_start).count();
			if (o.net.burst_ms > 0 and o.net.burst_every_s > 0 and
			    std::fmod(since, o.net.burst_every_s) < o.net.burst_ms / 1000.0)
			{
				res.net_dropped++;
				return false;
			}
			if (uni(rng) * 100 < o.net.loss_pct)
			{
				res.net_dropped++;
				return false;
			}
			auto at = now + std::chrono::microseconds(int64_t((o.net.delay_ms + o.net.jitter_ms * uni(rng)) * 1000));
			if (o.net.mbps > 0)
			{
				// serialisation behind the rate cap: each shard occupies the link for its bytes / rate
				auto start = std::max(rate_free, now);
				rate_free = start + std::chrono::nanoseconds(int64_t((shard->payload.size() + 60) * 8 * 1000 / o.net.mbps));
				at = std::max(at, rate_free);
			}
			at = std::max(at, last_release); // a Wi-Fi link does not reorder
			last_release = at;
			delayed.emplace_back(at, std::move(p));
			return false;
		};
		auto receive = [&](stream_socket * stream) -> th::packets {
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (not delayed.empty() and delayed.front().first <= clk::now())
				{
					from_control = false;
					auto p = std::move(delayed.front().second);
					delayed.pop_front();
					return p;
				}
				from_control = true;
				if (auto p = control.receive_pending())
					return std::move(*p);
				from_control = false;
				if (stream)
					if (auto p = stream->receive_pending())
					{
						if (shape(std::move(*p)))
							return std::move(*p);
						continue;
					}
				pollfd fds[2] = {{.fd = control.get_fd(), .events = POLLIN}, {.fd = stream ? stream->get_fd() : -1, .events = POLLIN}};
				int wait_ms = 50;
				if (not delayed.empty())
					wait_ms = int(std::clamp<int64_t>(
					        std::chrono::duration_cast<std::chrono::milliseconds>(delayed.front().first - clk::now()).count(), 0, 50));
				if (::poll(fds, 2, wait_ms) <= 0)
					continue;
				if (fds[0].revents & POLLIN)
				{
					// the last packet before a close (e.g. pin_check_4 with a wrong PIN) is still readable
					try
					{
						if (auto p = control.receive())
						{
							from_control = true;
							return std::move(*p);
						}
					}
					catch (wivrn::socket_shutdown &)
					{
						throw std::runtime_error("server closed the connection");
					}
				}
				else if (fds[0].revents & (POLLHUP | POLLERR))
					throw std::runtime_error("server closed the connection");
				if (stream and (fds[1].revents & POLLIN))
					if (auto p = stream->receive())
					{
						if (shape(std::move(*p)))
							return std::move(*p);
						continue;
					}
			}
			throw std::runtime_error("timeout");
		};

		// Client side of the crypto handshake: WiVRn v26.9 client/wivrn_client.cpp wivrn_session::handshake().
		crypto::key headset_keypair = o.private_key.empty() ? crypto::key::generate_x448_keypair()
		                                                    : crypto::key::from_private_key(o.private_key);
		control.send(fh::crypto_handshake{
		        .protocol_version = o.protocol_version ? o.protocol_version : wivrn::protocol_version,
		        .public_key = headset_keypair.public_key(),
		        .name = "steamac fake headset",
		});
		auto ch = std::get<th::crypto_handshake>(receive(nullptr));
		res.handshake_state = std::string(magic_enum::enum_name(ch.state));
		using cs = th::crypto_handshake::crypto_state;
		std::optional<secrets> sec;
		switch (ch.state)
		{
			case cs::encryption_disabled:
				break;
			case cs::pin_needed: {
				// the client's PIN prompt: wait for the user while watching the control socket (lobby.cpp pin_enter)
				for (auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.pin_delay_ms);
				     std::chrono::steady_clock::now() < until;)
				{
					pollfd pfd{.fd = control.get_fd(), .events = 0};
					if (::poll(&pfd, 1, 50) > 0 and (pfd.revents & (POLLHUP | POLLERR)))
						throw std::runtime_error("server closed the connection during PIN entry");
				}
				crypto::smp pin_check;
				auto msg1 = pin_check.step1(o.pin);
				control.send(fh::pin_check_1{msg1});
				auto msg2 = std::get<th::pin_check_2>(receive(nullptr)).message;
				auto msg3 = pin_check.step3(msg2);
				control.send(fh::pin_check_3{msg3});
				auto msg4 = std::get<th::pin_check_4>(receive(nullptr)).message;
				if (not pin_check.step5(msg4))
					throw std::runtime_error("Incorrect PIN");
			}
				[[fallthrough]];
			case cs::client_already_paired: {
				crypto::key server_key = crypto::key::from_public_key(ch.public_key);
				sec.emplace(headset_keypair, server_key, ch.state == cs::pin_needed ? o.pin : std::string("000000"));
				control.set_aes_key_and_ivs(sec->control_key, sec->control_iv_to_headset, sec->control_iv_from_headset);
				res.encrypted = true;
				break;
			}
			default:
				return res;
		}
		control.send(fh::crypto_handshake{});
		auto h = std::get<th::handshake>(receive(nullptr));
		std::unique_ptr<stream_socket> stream;
		if (h.stream_port > 0)
		{
			if (ipv4)
			{
				// The Android client connects its AF_INET6 UDP socket to the sockaddr_in (Linux maps it to IPv4);
				// macOS refuses that, so use an AF_INET socket: same packets on the wire.
				int fd = socket(AF_INET, SOCK_DGRAM, 0);
				if (fd < 0)
					throw std::system_error(errno, std::system_category());
				stream = std::make_unique<stream_socket>(fd);
			}
			else
				stream = std::make_unique<stream_socket>();
			if (sec)
				stream->set_aes_key_and_ivs(sec->stream_key, sec->stream_iv_header_to_headset, sec->stream_iv_header_from_headset);
			addr.sin6_port = htons(h.stream_port);
			addr4.sin_port = htons(h.stream_port);
			if (ipv4)
				stream->connect(addr4);
			else
				stream->connect(addr);
			stream->send(fh::handshake{});
		}
		else
			control.send(fh::handshake{});
		std::get<th::handshake>(receive(stream.get()));

		fh::headset_info_packet info{};
		info.render_eye_width = o.render_width ? o.render_width : o.eye_width;
		info.render_eye_height = o.render_height ? o.render_height : o.eye_height;
		info.stream_eye_width = o.eye_width;
		info.stream_eye_height = o.eye_height;
		info.available_refresh_rates = {72, 90};
		info.settings.preferred_refresh_rate = o.refresh_hz;
		info.settings.bitrate_bps = 20'000'000;
		for (int e = 0; e < 2; ++e)
			info.fov[e] = XrFovf{o.fov[e][0], o.fov[e][1], o.fov[e][2], o.fov[e][3]};
		info.supported_codecs = {wivrn::video_codec::h265, wivrn::video_codec::h264};
		info.system_name = "fake";
		if (o.speaker_channels)
			info.speaker = fh::headset_info_packet::audio_description{o.speaker_channels, o.speaker_rate};
		if (o.mic_channels)
			info.microphone = fh::headset_info_packet::audio_description{o.mic_channels, o.mic_rate};
		control.send(std::move(info));
		if (o.battery_charge >= 0)
			control.send(fh::battery{.charge = o.battery_charge, .present = true, .charging = true});
		if (not o.start_app.empty())
		{
			control.send(fh::get_application_list{.language = "en"});
			control.send(fh::start_app{.app_id = o.start_app});
		}

		std::array<VtDecoder, 2> dec{VtDecoder(not o.expect_h264), VtDecoder(not o.expect_h264)};
		std::array<std::map<uint64_t, pending_frame>, 2> frames;
		std::array<bool, 2> need_idr{true, true};
		bool dropped = false;
		std::array<std::optional<uint64_t>, 2> last_frame; // newest frame index seen per stream
		auto last_tracking = std::chrono::steady_clock::now();
		float t = 0;
		int input_tick = 0;

		std::array<std::map<uint64_t, int64_t>, 2> first_packet; // frame -> headset time of its first shard
		auto send_feedback = [&](uint8_t idx, uint64_t frame, bool ok) {
			fh::feedback fb{};
			fb.frame_index = frame;
			fb.stream_index = idx;
			auto fp = first_packet[idx].find(frame);
			fb.received_first_packet = fp != first_packet[idx].end() ? fp->second : hs_now();
			fb.received_last_packet = ok ? hs_now() : 0;
			fb.sent_to_decoder = ok ? hs_now() : 0;
			fb.received_from_decoder = ok ? std::max(hs_now(), fb.received_first_packet + o.decode_ns) : 0;
			if (ok and o.display_period_ns > 0)
			{
				// blit at the next headset vsync after decoding, displayed one vsync later
				int64_t t = fb.received_from_decoder - o.display_phase_ns;
				int64_t blit = (t / o.display_period_ns + 1) * o.display_period_ns + o.display_phase_ns;
				fb.blitted = blit;
				fb.displayed = blit + o.display_period_ns;
				fb.times_displayed = 1;
			}
			control.send(std::move(fb));
		};

		bool mic_on = false;
		int64_t mic_start = 0;
		auto send_mic = [&] {
			if (not mic_on or not o.mic_channels)
				return;
			int64_t now = vrhost::now_ns();
			size_t due = size_t((now - mic_start) * double(o.mic_rate) / 1e9);
			size_t marker_at = size_t(o.mic_rate) * o.mic_marker_ms / 1000;
			while (res.mic_frames_sent + o.mic_rate / 200 <= due)
			{
				size_t n = o.mic_rate / 200;
				std::vector<int16_t> pcm(n * o.mic_channels);
				for (size_t f = 0; f < n; ++f)
				{
					size_t k = res.mic_frames_sent + f;
					int16_t v = int16_t(std::lround(8000 * std::sin(2 * M_PI * o.mic_tone_hz * double(k) / o.mic_rate)));
					if (k >= marker_at and k < marker_at + 48)
					{
						v = 30000;
						if (k == marker_at)
							res.mic_marker_sent_ns = vrhost::now_ns();
					}
					for (int c = 0; c < o.mic_channels; ++c)
						pcm[f * o.mic_channels + c] = v;
				}
				res.mic_frames_sent += n;
				control.send(wivrn::audio_data{
				        .timestamp = hs_now(),
				        .payload = std::span(reinterpret_cast<uint8_t *>(pcm.data()), pcm.size() * 2),
				});
			}
		};

		while (res.decoded[0] < o.frames or res.decoded[1] < o.frames)
		{
			send_mic();
			auto now = std::chrono::steady_clock::now();
			if (now - last_tracking > std::chrono::milliseconds(5))
			{
				last_tracking = now;
				t += 0.005f;
				fh::tracking tr{};
				tr.production_timestamp = hs_now();
				tr.timestamp = hs_now();
				tr.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
				for (int i = 0; i < 2; ++i)
					tr.views[i] = {.pose = {.orientation = {0, 0, 0, 1}, .position = {i ? 0.032f : -0.032f, 0, 0}},
					               .fov = {o.fov[i][0], o.fov[i][1], o.fov[i][2], o.fov[i][3]}};
				// t advances 1 per second of motion phase: the default (0.159 Hz) is the old 0.3 sin(t) yaw
				float w = 2 * float(M_PI) * o.motion_hz, ph = w * t;
				float yaw = o.yaw_amplitude * std::sin(ph), pitch = o.pitch_amplitude * std::sin(ph + float(M_PI) / 2);
				XrPosef head = yaw_pose(yaw);
				if (o.pitch_amplitude != 0)
				{
					// yaw about +y, then pitch about the head's x: q = q_yaw * q_pitch
					float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
					head.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
				}
				tr.device_poses.push_back({.pose = head,
				                           .angular_velocity = {0, o.yaw_amplitude * w * std::cos(ph), 0},
				                           .device = wivrn::device_id::HEAD,
				                           .flags = wivrn::from_headset::orientation_valid | wivrn::from_headset::position_valid |
				                                    wivrn::from_headset::angular_velocity_valid});
				if (o.controllers)
				{
					// hands 40 cm in front, 30 cm below the head, 20 cm to each side, swaying with the head
					for (auto [dev, x]: {std::pair{wivrn::device_id::LEFT_GRIP, -0.2f}, {wivrn::device_id::LEFT_AIM, -0.2f},
					                     {wivrn::device_id::RIGHT_GRIP, 0.2f}, {wivrn::device_id::RIGHT_AIM, 0.2f}})
						tr.device_poses.push_back({.pose = {.orientation = head.orientation, .position = {x + 0.1f * std::sin(ph), 1.3f, -0.4f}},
						                           .device = dev,
						                           .flags = wivrn::from_headset::orientation_valid | wivrn::from_headset::position_valid |
						                                    wivrn::from_headset::orientation_tracked |
						                                    wivrn::from_headset::position_tracked});
				}
				if (stream)
					stream->send(std::move(tr));
				else
					control.send(std::move(tr));
				if (o.controllers and ++input_tick % 4 == 0)
				{
					// 50 Hz: right trigger ramps 0..1 over 2 s, A pressed for the first half of every second, left stick x sways
					fh::inputs in{};
					float trig = std::fmod(t, 2.f) / 2;
					bool a = std::fmod(t, 1.f) < 0.5f;
					XrTime now = hs_now();
					in.values = {{wivrn::device_id::RIGHT_TRIGGER_VALUE, trig, now},
					             {wivrn::device_id::RIGHT_TRIGGER_TOUCH, trig > 0.05f ? 1.f : 0.f, now},
					             {wivrn::device_id::A_CLICK, a ? 1.f : 0.f, now},
					             // B (VRChat: menu) held for 0.3 s every 8 s
					             {wivrn::device_id::B_CLICK, std::fmod(t, 8.f) < 0.3f ? 1.f : 0.f, now},
					             {wivrn::device_id::LEFT_THUMBSTICK_X, 0.5f * std::sin(ph), now},
					             {wivrn::device_id::LEFT_SQUEEZE_VALUE, a ? 0.f : 1.f, now}};
					control.send(std::move(in));
				}
			}

			th::packets pkt = receive(stream.get());
			if (auto * q = std::get_if<th::timesync_query>(&pkt))
			{
				res.timesync_queries++;
				fh::timesync_response r{.query = q->query, .response = hs_now()};
				if (stream)
					stream->send(std::move(r));
				else
					control.send(std::move(r));
			}
			else if (std::holds_alternative<th::video_stream_description>(pkt))
			{
				auto & d = std::get<th::video_stream_description>(pkt);
				res.got_stream_description = d.width == o.eye_width and d.height == o.eye_height and
				                             d.codec[0] == (o.expect_h264 ? wivrn::video_codec::h264 : wivrn::video_codec::h265);
			}
			else if (auto * ad = std::get_if<th::audio_stream_description>(&pkt))
			{
				bool spk = o.speaker_channels ? ad->speaker and ad->speaker->num_channels == o.speaker_channels and
				                                        ad->speaker->sample_rate == o.speaker_rate
				                              : not ad->speaker;
				bool mic = o.mic_channels ? ad->microphone and ad->microphone->num_channels == o.mic_channels and
				                                    ad->microphone->sample_rate == o.mic_rate
				                          : not ad->microphone;
				res.got_audio_description = spk and mic;
			}
			else if (auto * fc = std::get_if<th::feature_control>(&pkt))
			{
				if (fc->f == th::feature_control::microphone)
				{
					res.mic_feature.push_back(fc->state);
					if (fc->state and not mic_on)
						mic_start = vrhost::now_ns();
					mic_on = fc->state;
				}
			}
			else if (auto * a = std::get_if<wivrn::audio_data>(&pkt))
			{
				const int16_t * p = reinterpret_cast<const int16_t *>(a->payload.data());
				size_t samples = a->payload.size() / 2;
				res.speaker.insert(res.speaker.end(), p, p + samples);
				res.speaker_packets++;
				if (o.speaker_channels)
				{
					res.max_speaker_packet_frames = std::max(res.max_speaker_packet_frames, int(samples / o.speaker_channels));
					res.speaker_arrivals.emplace_back(vrhost::now_ns(), res.speaker.size() / o.speaker_channels);
				}
			}
			else if (auto * al = std::get_if<th::application_list>(&pkt))
			{
				res.app_ids.clear();
				for (auto & a: al->applications)
					res.app_ids.push_back(a.id);
			}
			else if (auto * rc = std::get_if<th::refresh_rate_change>(&pkt))
			{
				res.refresh_changes.push_back(rc->hz);
				control.send(fh::refresh_rate_changed{.from = o.refresh_hz, .to = rc->hz});
			}
			else if (auto * hp = std::get_if<th::haptics>(&pkt))
				res.haptics.push_back({int(hp->id), int64_t(hp->duration.count()), hp->frequency, hp->amplitude});
			else if (std::holds_alternative<th::tracking_control>(pkt))
				res.got_tracking_control = not std::get<th::tracking_control>(pkt).pattern.empty();
			else if (auto * s = std::get_if<th::video_stream_data_shard>(&pkt))
			{
				res.shards++;
				if (from_control and stream)
					res.control_shards++;
				res.max_shard_bytes = std::max(res.max_shard_bytes, int(s->payload.size()));
				uint8_t idx = s->stream_item_idx;
				if (idx > 1)
					continue;
				if (o.drop_frame >= 0 and idx == 0 and int64_t(s->frame_idx) == o.drop_frame)
				{
					if (not dropped)
					{
						dropped = true;
						need_idr[0] = true; // decoder state lost until the next IDR
						send_feedback(0, s->frame_idx, false);
					}
					continue;
				}
				// WiVRn's shard accumulator: a newer frame's shard ends the wait for the indices before it, which are
				// reported as "not sent because no shard was received"
				if (last_frame[idx] and s->frame_idx > *last_frame[idx] + 1)
					for (uint64_t lost = *last_frame[idx] + 1; lost < s->frame_idx and lost < *last_frame[idx] + 9; ++lost)
					{
						res.index_gaps[idx]++;
						send_feedback(idx, lost, false);
					}
				if (not last_frame[idx] or s->frame_idx > *last_frame[idx])
					last_frame[idx] = s->frame_idx;
				first_packet[idx].try_emplace(s->frame_idx, hs_now());
				if (first_packet[idx].size() > 64)
					first_packet[idx].erase(first_packet[idx].begin());
				auto & f = frames[idx][s->frame_idx];
				f.shards[s->shard_idx].assign(s->payload.begin(), s->payload.end());
				if (s->view_info)
					f.view_info = s->view_info;
				if (s->timing_info)
					f.last = s->shard_idx;
				if (f.last < 0 or int(f.shards.size()) != f.last + 1 or not f.view_info)
					continue;

				std::vector<uint8_t> au;
				for (auto & [_, d]: f.shards)
					au.insert(au.end(), d.begin(), d.end());
				auto vi = *f.view_info;
				uint64_t frame_idx = s->frame_idx;
				frames[idx].erase(frames[idx].begin(), std::next(frames[idx].find(frame_idx)));

				bool is_idr = false;
				for (auto n: vrhost::annexb::split(au))
					is_idr |= o.expect_h264 ? (n[0] & 0x1f) == 7 : vrhost::annexb::hevc_nal_type(n) == vrhost::annexb::hevc_vps; // VPS / SPS
				if (is_idr)
				{
					res.idr[idx]++;
					if (dropped and idx == 0)
						res.idr_after_drop++;
					need_idr[idx] = false;
				}
				if (need_idr[idx])
				{
					send_feedback(idx, frame_idx, false);
					continue;
				}

				// view_info checks: identity foveation, both eye poses 64 mm apart, display time in headset clock
				for (int e = 0; e < 2; ++e)
				{
					res.foveation_x[e] = vi.foveation[e].x;
					res.foveation_y[e] = vi.foveation[e].y;
				}
				for (int e = 0; e < 2 and o.identity_foveation; ++e)
					if (vi.foveation[e].x != std::vector<uint16_t>{o.eye_width} or vi.foveation[e].y != std::vector<uint16_t>{o.eye_height})
						res.view_info_ok = false;
				// (frames rendered before the first tracking packet arrived carry the default, all-zero view poses)
				bool tracked = vi.pose[0].position.x != 0 or vi.pose[1].position.x != 0;
				float dx = vi.pose[1].position.x - vi.pose[0].position.x, dz = vi.pose[1].position.z - vi.pose[0].position.z;
				if (tracked and std::abs(std::sqrt(dx * dx + dz * dz) - 0.064f) > 0.002f)
					res.view_info_ok = false;
				res.display_time_offset_ms = (vi.display_time - hs_now()) / 1e6;
				if (idx == 0)
				{
					res.display_times.emplace_back(frame_idx, vi.display_time);
					auto & q = vi.pose[0].orientation;
					res.view_orientation.push_back({frame_idx, {q.x, q.y, q.z, q.w}});
				}

				CVPixelBufferRef img = dec[idx].decode(au);
				send_feedback(idx, frame_idx, img != nullptr);
				if (not img)
					continue;
				int64_t read = vrhost::TestPattern::read_frame_index(img);
				if (o.on_decoded)
					o.on_decoded(idx, frame_idx, img);
				if (o.on_view)
				{
					FakeHeadsetOptions::View v;
					for (int e = 0; e < 2; ++e)
					{
						auto & q = vi.pose[e].orientation;
						auto & p = vi.pose[e].position;
						v.orientation[e] = {q.x, q.y, q.z, q.w};
						v.position[e] = {p.x, p.y, p.z};
						v.fov[e] = {vi.fov[e].angleLeft, vi.fov[e].angleRight, vi.fov[e].angleUp, vi.fov[e].angleDown};
						v.foveation_x[e] = vi.foveation[e].x;
						v.foveation_y[e] = vi.foveation[e].y;
					}
					o.on_view(idx, frame_idx, img, v);
				}
				if (read != int64_t(frame_idx & 0xffff))
					res.index_mismatch[idx]++;
				res.decoded[idx]++;
				if (dropped and idx == 0)
					res.decoded_after_drop++;
				CVPixelBufferRelease(img);
			}
		}
	}
	catch (std::exception & e)
	{
		res.error = e.what();
	}
	return res;
}
