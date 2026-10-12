// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_session.h"

#include "encoder/vt_encoder.h"
#include "session/clock.h"
#include "session/pose_history.h"
#include "vd/vd_accounts.h"
#include "vd/vd_channel.h"
#include "vd/vd_discovery.h"
#include "vd/vd_settings.h"
#include "vd/vd_wire.h"
#include "wivrn/annexb.h"
#include "wivrn/input_map.h"
#include "wivrn/known_keys.h"

#include <lz4.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
static_assert(XXH_VERSION_NUMBER >= 800, "XXH3 needs xxHash >= 0.8 (lz4's bundled xxhash.h shadows it)");

#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace vrhost
{
using namespace vd;

namespace
{
enum Ch
{
	control,
	data,
	video,
	audio,
	channel_count,
};
const char * channel_name[] = {"control", "data", "video", "audio"};

// TODO(vd-live:fov): Quest 3-like half-angles (radians) until the headset reports its own.
constexpr std::array<Fov, 2> default_fov{Fov{-0.96f, 0.82f, 0.87f, -0.96f}, Fov{-0.82f, 0.96f, 0.87f, -0.96f}};

FovPort to_fov_port(const Fov & f)
{
	return {std::tan(f.up), std::tan(-f.down), std::tan(-f.left), std::tan(f.right)};
}

std::vector<uint8_t> length_prefixed(std::span<const uint8_t> annexb_stream)
{
	std::vector<uint8_t> out;
	for (auto nal: annexb::split(annexb_stream))
	{
		uint32_t n = uint32_t(nal.size());
		uint8_t len[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
		out.insert(out.end(), len, len + 4);
		out.insert(out.end(), nal.begin(), nal.end());
	}
	return out;
}

std::string host_name()
{
	char b[256] = {};
	gethostname(b, sizeof b - 1);
	std::string s = b;
	if (auto dot = s.find('.'); dot != std::string::npos)
		s.resize(dot);
	return s.empty() ? "Mac" : s;
}

std::vector<std::string> private_addresses()
{
	std::vector<std::string> out;
	ifaddrs * list = nullptr;
	if (getifaddrs(&list) != 0)
		return out;
	for (auto i = list; i; i = i->ifa_next)
		if (i->ifa_addr and i->ifa_addr->sa_family == AF_INET and (i->ifa_flags & IFF_UP) and not(i->ifa_flags & IFF_LOOPBACK))
		{
			char s[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, s, sizeof s);
			out.push_back(s);
		}
	freeifaddrs(list);
	return out;
}
} // namespace

struct VdSession::impl
{
	VdConfig cfg;
	VdStats stats;
	SessionListener * listener = nullptr;
	AesKey session_key = AesKey::random();
	std::array<uint8_t, 16> guid{};
	AccountStore accounts;
	std::atomic<bool> pairing{false};

	int listen_fd[channel_count] = {-1, -1, -1, -1};
	int udp_fd = -1;
	Ports bound{};
	std::atomic<bool> quit{false};
	std::thread accept_thread;
	std::mutex threads_mutex;
	std::vector<std::thread> readers;

	mutable std::mutex m; // channels, settings, headset state
	std::array<std::shared_ptr<Channel>, channel_count> ch;
	Settings user, mobile, streamer;
	std::optional<HeadsetInfo> info;
	bool annexb_out = false;
	std::atomic<bool> is_connected{false};
	std::atomic<uint32_t> divisor{1};
	std::atomic<float> refresh{90};

	std::mutex encode_mutex;
	std::array<std::unique_ptr<VtEncoder>, 2> encoders;
	std::atomic<bool> idr_pending{true};
	int64_t wire_index = 0;
	ns last_submit = 0;

	std::mutex pace_mutex;
	ns next_wake = 0;
	uint64_t frame_id = 0;

	std::array<PoseHistory, size_t(Device::count)> history;
	std::mutex haptic_mutex;
	std::array<float, 2> haptic_amp{};
	std::array<ns, 2> haptic_until{};
	std::array<std::array<float, svr::count>, 2> last_input{};
	std::atomic<bool> mic_on{false};

	explicit impl(VdConfig c) :
	        cfg(std::move(c)),
	        accounts(cfg.accounts_file.empty() ? KnownKeys::default_dir() / "vd_accounts" : std::filesystem::path(cfg.accounts_file))
	{
		random_bytes(guid);
		cfg.bitrate_bps = std::min(cfg.bitrate_bps, max_bitrate_bps);
		if (not cfg.discovery_key)
			cfg.encrypt_local = false; // the session key travels in the discovery reply
		pairing = cfg.discovery_key and (cfg.pairing or accounts.list().empty());
		divisor = cfg.spacewarp == VdConfig::Spacewarp::on ? 2 : 1;
		streamer.set(key_allow_extrapolation, true);
		streamer.set(key_vr_available, true);
		streamer.set("EncryptLocalTraffic", cfg.encrypt_local);
	}

	void log(const std::string & s)
	{
		if (cfg.log)
			cfg.log(s);
	}

	void spawn(std::function<void()> f)
	{
		std::lock_guard lock(threads_mutex);
		readers.emplace_back(std::move(f));
	}

	// --- discovery ---------------------------------------------------------------------------------------------
	ComputerRecord record() const
	{
		ComputerRecord r;
		r.id = guid_text(guid);
		r.name = cfg.computer_name.empty() ? host_name() : cfg.computer_name;
		r.addresses = cfg.addresses.empty() ? private_addresses() : cfg.addresses;
		r.control_port = bound.control;
		r.data_port = bound.data;
		r.video_port = bound.video;
		r.audio_port = bound.audio;
		r.discovery_port = bound.discovery;
		r.encrypt_local = cfg.encrypt_local;
		return r;
	}

	void on_datagram()
	{
		uint8_t buf[65536];
		sockaddr_storage from{};
		socklen_t from_len = sizeof from;
		ssize_t n = recvfrom(udp_fd, buf, sizeof buf, 0, (sockaddr *)&from, &from_len);
		if (n <= 0 or not cfg.discovery_key)
			return;
		auto plain = aes_unwrap(*cfg.discovery_key, std::span(buf, size_t(n)));
		if (not plain)
		{
			stats.bad_messages++;
			return;
		}
		if (auto c = parse_connect_now(*plain))
		{
			// TODO(vd-live:connect-now): on the LAN the headset connects to us; nothing to start.
			if (c->guid == guid)
				stats.connect_now++;
			return;
		}
		auto offer = parse_offer(*plain);
		if (not offer)
		{
			stats.bad_messages++;
			return;
		}
		stats.offers++;
		uint16_t platform = offer->platform.value_or(0);
		if (not accounts.allowed(platform, offer->account))
		{
			if (not pairing.exchange(false)) // a pairing window admits one new account, like the Streamer's prompt
			{
				stats.offers_refused++;
				log("discovery: offer from an account that is not allowed (turn pairing on to allow it)");
				return;
			}
			log("discovery: new account allowed (pairing)");
		}
		accounts.allow(platform, offer->account);
		auto reply = encode_reply(*cfg.discovery_key, session_key, offer->rsa_public_xml, record());
		if (not reply)
		{
			stats.bad_messages++;
			log("discovery: unusable RSA key in offer");
			return;
		}
		sendto(udp_fd, reply->data(), reply->size(), 0, (sockaddr *)&from, from_len);
	}

	// --- channels ----------------------------------------------------------------------------------------------
	void accept_loop()
	{
		std::vector<pollfd> fds;
		for (int c = 0; c < channel_count; ++c)
			fds.push_back({listen_fd[c], POLLIN, 0});
		if (udp_fd >= 0)
			fds.push_back({udp_fd, POLLIN, 0});
		while (not quit)
		{
			if (poll(fds.data(), fds.size(), 100) <= 0)
				continue;
			for (int c = 0; c < channel_count; ++c)
				if (fds[c].revents & POLLIN)
					accept_channel(Ch(c));
			if (udp_fd >= 0 and (fds.back().revents & POLLIN))
				on_datagram();
		}
	}

	void accept_channel(Ch c)
	{
		int fd = accept(listen_fd[c], nullptr, nullptr);
		if (fd < 0)
			return;
		tune_socket(fd, c == video);
		auto chan = std::make_shared<Channel>(fd);
		// One headset at a time: a new connection replaces the channel's previous one (its reader then ends without
		// touching the session); a closing control channel ends the whole session.
		if (c != control and cfg.encrypt_local)
			chan->encrypt(session_key);
		{
			std::lock_guard lock(m);
			if (ch[c])
				ch[c]->shutdown();
			ch[c] = chan;
		}
		log(std::string("vd: ") + channel_name[c] + " channel connected");
		switch (c)
		{
			case control: spawn([this, chan] { control_loop(chan); }); break;
			case data: spawn([this, chan] { data_loop(chan); }); break;
			case audio: spawn([this, chan] { audio_loop(chan); }); break;
			case video: spawn([this, chan] { drain_loop(chan); }); break;
			default: break;
		}
		update_connection();
	}

	void channel_closed(Ch c, const std::shared_ptr<Channel> & chan)
	{
		{
			std::lock_guard lock(m);
			if (ch[c] != chan)
				return; // replaced already
			ch[c].reset();
		}
		log(std::string("vd: ") + channel_name[c] + " channel closed");
		if (c == control)
			drop_session("control channel closed");
		update_connection();
	}

	void drop_session(const char * why)
	{
		std::array<std::shared_ptr<Channel>, channel_count> old;
		{
			std::lock_guard lock(m);
			old = std::exchange(ch, {});
			info.reset();
			mobile = {};
			user = {};
		}
		bool any = false;
		for (auto & c: old)
			if (c)
			{
				c->shutdown();
				any = true;
			}
		if (any)
			log(std::string("vd: session dropped: ") + why);
		update_connection();
	}

	// connected = control + data + video up and the headset's mobile settings known.
	void update_connection()
	{
		bool now_connected;
		std::optional<HeadsetInfo> h;
		std::shared_ptr<Channel> v;
		{
			std::lock_guard lock(m);
			now_connected = info and ch[control] and ch[data] and ch[video];
			h = info;
			v = ch[video];
		}
		bool was = is_connected.exchange(now_connected);
		if (now_connected and not was)
		{
			stats.sessions++;
			{
				std::lock_guard lock(encode_mutex); // a new headset may want another codec/size/rate
				encoders = {};
				last_submit = 0;
			}
			idr_pending = true;
			send_video_format(*v, *h);
			log("vd: headset connected: " + h->name + " " + std::to_string(h->eye_width) + "x" + std::to_string(h->eye_height) +
			    " @ " + std::to_string(int(h->preferred_refresh_rate)) + " Hz, render divisor " + std::to_string(divisor.load()));
			if (listener)
				listener->on_connected(*h);
		}
		else if (was and not now_connected)
		{
			for (auto & hist: history)
				hist.clear();
			if (listener)
				listener->on_disconnected();
		}
	}

	// --- control channel ---------------------------------------------------------------------------------------
	void control_loop(std::shared_ptr<Channel> chan)
	{
		try
		{
			// PeerInfo exchange (TODO(vd-live:peer-info)): the headset's first, then ours, then encryption.
			uint8_t family = 0;
			if (not chan->read_exact(&family, 1, 10000) or (family != 4 and family != 16))
				throw std::runtime_error("no PeerInfo");
			std::vector<uint8_t> rest(family + 5);
			if (not chan->read_exact(rest.data(), rest.size(), 10000))
				throw std::runtime_error("short PeerInfo");
			sockaddr_storage local{};
			socklen_t len = sizeof local;
			getsockname(chan->fd(), (sockaddr *)&local, &len);
			PeerInfo mine{.port = bound.control, .encrypt = cfg.encrypt_local};
			auto l6 = (sockaddr_in6 *)&local;
			if (IN6_IS_ADDR_V4MAPPED(&l6->sin6_addr))
				mine.address.assign(l6->sin6_addr.s6_addr + 12, l6->sin6_addr.s6_addr + 16);
			else
				mine.address.assign(l6->sin6_addr.s6_addr, l6->sin6_addr.s6_addr + 16);
			if (not chan->send(encode_peer_info(mine)))
				throw std::runtime_error("send PeerInfo");
			if (cfg.encrypt_local)
				chan->encrypt(session_key);
			{
				std::lock_guard lock(m);
				auto json = streamer.to_json();
				chan->send(control_message(Msg::reload_streamer_settings, {(const uint8_t *)json.data(), json.size()}));
			}
			while (not quit)
			{
				uint8_t hdr[5];
				if (not chan->read_exact(hdr, 5))
					break;
				uint32_t n = uint32_t(hdr[1]) << 24 | uint32_t(hdr[2]) << 16 | uint32_t(hdr[3]) << 8 | hdr[4];
				if (n > (16u << 20))
					throw std::runtime_error("control message too large");
				std::vector<uint8_t> payload(n);
				if (n and not chan->read_exact(payload.data(), n))
					break;
				on_control(Msg(hdr[0]), payload);
			}
		}
		catch (std::exception & e)
		{
			stats.bad_messages++;
			log(std::string("vd: control: ") + e.what());
		}
		channel_closed(control, chan);
	}

	void on_control(Msg type, std::span<const uint8_t> payload)
	{
		std::string text(payload.begin(), payload.end());
		switch (type)
		{
			case Msg::reload_user_settings:
			case Msg::reload_mobile_settings:
			case Msg::reload_streamer_settings: {
				stats.settings_messages++;
				std::lock_guard lock(m);
				Settings & s = type == Msg::reload_user_settings ? user : type == Msg::reload_mobile_settings ? mobile : streamer;
				if (not s.parse(text))
					stats.bad_messages++;
				break;
			}
			case Msg::user_setting_changed:
			case Msg::mobile_setting_changed:
			case Msg::streamer_setting_changed: {
				stats.settings_messages++;
				BeReader r(payload);
				std::string name = r.str(), value = r.str();
				std::lock_guard lock(m);
				Settings & s = type == Msg::user_setting_changed ? user : type == Msg::mobile_setting_changed ? mobile : streamer;
				s.set_raw(name, value);
				break;
			}
			case Msg::force_key_frame:
				stats.idr_requests++;
				idr_pending = true;
				return;
			case Msg::disconnect:
			case Msg::exit:
				drop_session("headset disconnected");
				return;
			default: // restart stream, recenter, the desktop features: accepted and ignored
				return;
		}
		if (type == Msg::reload_mobile_settings or type == Msg::mobile_setting_changed)
			apply_mobile();
	}

	// Headset description from the mobile settings (TODO(vd-live:settings-keys), TODO(vd-live:preset-sizes)).
	void apply_mobile()
	{
		HeadsetInfo h;
		uint32_t d;
		float old_refresh = refresh;
		uint32_t old_divisor = divisor;
		std::shared_ptr<Channel> v;
		bool format_changed;
		{
			std::lock_guard lock(m);
			auto hmd = mobile.string(key_hmd_type);
			h.name = hmd ? *hmd : "Virtual Desktop headset";
			h.eye_width = cfg.width ? cfg.width : quest3_low_eye;
			h.eye_height = cfg.height ? cfg.height : quest3_low_eye;
			h.render_eye_width = h.eye_width; // no host foveation: the headset upscales the decoded stream
			h.render_eye_height = h.eye_height;
			h.foveation = FoveationModel::none;
			h.refresh_rates = {72, 80, 90, 120};
			float want = float(mobile.number(key_framerate).value_or(90));
			h.preferred_refresh_rate = std::ranges::contains(h.refresh_rates, want) ? want : 90;
			h.fov = default_fov;
			auto codec = mobile.string(key_codec).value_or("HEVC");
			h.codecs = codec == "H264" or codec == "H264Plus" ? std::vector{Codec::h264} : std::vector{Codec::hevc, Codec::h264};
			h.speaker = AudioFormat{speaker_channels, audio_rate};
			h.microphone = AudioFormat{mic_channels, audio_rate};
			h.max_bitrate_bps = max_bitrate_bps;
			uint32_t bitrate = cfg.bitrate_bps;
			if (auto mbps = mobile.number(key_bitrate); mbps and *mbps > 0)
				bitrate = std::min(bitrate, uint32_t(std::min(*mbps * 1e6, double(max_bitrate_bps))));
			h.bitrate_bps = bitrate;
			annexb_out = mobile.boolean(key_annexb).value_or(false);
			bool ssw = cfg.spacewarp == VdConfig::Spacewarp::on or
			           (cfg.spacewarp == VdConfig::Spacewarp::headset and mobile.boolean(key_spacewarp).value_or(false));
			d = ssw ? 2 : 1;
			format_changed = info and (info->eye_width != h.eye_width or info->eye_height != h.eye_height or
			                           info->codecs != h.codecs or info->preferred_refresh_rate != h.preferred_refresh_rate);
			info = h;
			v = ch[video];
		}
		refresh = h.preferred_refresh_rate;
		divisor = d;
		if (format_changed)
		{
			std::lock_guard lock(encode_mutex);
			encoders = {};
			idr_pending = true;
		}
		if (is_connected)
		{
			if (format_changed and v)
				send_video_format(*v, h);
			if (listener and old_refresh != h.preferred_refresh_rate)
				listener->on_refresh_rate(h.preferred_refresh_rate);
		}
		if (listener and old_divisor != d)
			listener->on_render_rate(d);
		update_connection();
	}

	void send_video_format(Channel & v, const HeadsetInfo & h)
	{
		VideoFormat f;
		f.source = Source::vr;
		f.codec = h.codecs.front() == Codec::hevc ? VideoCodec::hevc : VideoCodec::h264;
		f.frame_rate = h.preferred_refresh_rate; // TODO(vd-live:ssw-cadence): display rate, not the render rate
		f.width = int32_t(h.eye_width);
		f.height = int32_t(h.eye_height);
		f.stream_count = 2;
		for (int e = 0; e < 2; ++e)
			f.stream[e] = {f.width, f.height, {e * f.width, 0, f.width, f.height}};
		std::vector<uint8_t> msg{tag_video_format};
		auto b = bytes_of(f);
		msg.insert(msg.end(), b.begin(), b.end());
		v.send(msg);
	}

	// Headset -> host on the video channel is only TCP ACKs; reading tells us when it closes.
	void drain_loop(std::shared_ptr<Channel> chan)
	{
		uint8_t b;
		while (not quit and chan->read_exact(&b, 1))
			;
		channel_closed(video, chan);
	}

	// --- data channel: tracking + input up ---------------------------------------------------------------------
	void data_loop(std::shared_ptr<Channel> chan)
	{
		try
		{
			while (not quit)
			{
				uint8_t prefix[16];
				if (not chan->read_exact(prefix, 16))
					break;
				int32_t comp, raw;
				uint64_t hash;
				memcpy(&comp, prefix, 4);
				memcpy(&raw, prefix + 4, 4);
				memcpy(&hash, prefix + 8, 8);
				if (comp <= 0 or raw <= 0 or comp > (1 << 20) or raw > (1 << 20))
					throw std::runtime_error("bad FrameData sizes");
				std::vector<uint8_t> in(static_cast<size_t>(comp)), out(static_cast<size_t>(raw));
				if (not chan->read_exact(in.data(), in.size()))
					break;
				int n = LZ4_decompress_safe((const char *)in.data(), (char *)out.data(), comp, raw);
				if (n != raw or XXH3_64bits(out.data(), out.size()) != hash)
				{
					stats.bad_messages++;
					continue;
				}
				// The struct is sent truncated to the active feature set: shorter = base only, longer = extra blocks.
				FrameData fd{};
				memcpy(&fd, out.data(), std::min(out.size(), sizeof fd));
				on_frame_data(fd);
			}
		}
		catch (std::exception & e)
		{
			stats.bad_messages++;
			log(std::string("vd: data: ") + e.what());
		}
		channel_closed(data, chan);
	}

	static TrackedPose tracked(Device d, const TrackedDevice & t, ns time)
	{
		TrackedPose p{.device = d, .time = time};
		p.pose.orientation = {t.orientation.x, t.orientation.y, t.orientation.z, t.orientation.w};
		p.pose.position = {t.position.x, t.position.y, t.position.z};
		p.linear_velocity = {t.linear_velocity.x, t.linear_velocity.y, t.linear_velocity.z};
		p.angular_velocity = {t.angular_velocity.x, t.angular_velocity.y, t.angular_velocity.z};
		p.orientation_valid = t.orientation_tracked;
		p.position_valid = t.position_tracked;
		return p;
	}

	void on_frame_data(const FrameData & fd)
	{
		ns t = now_ns(); // TODO(vd-live:clock): no clock sync in the notes; the sample time is the arrival time
		std::array<TrackedPose, 5> poses{
		        tracked(Device::head, fd.head, t),
		        tracked(Device::left_grip, fd.controller[0], t),
		        tracked(Device::left_aim, fd.controller[0], t), // TODO(vd-live:aim): aim = grip until an offset is known
		        tracked(Device::right_grip, fd.controller[1], t),
		        tracked(Device::right_aim, fd.controller[1], t),
		};
		for (auto & p: poses)
			history[size_t(p.device)].add(p);
		stats.tracking++;
		if (listener)
			listener->on_tracking(poses);

		std::vector<InputValue> changed;
		for (int hand = 0; hand < 2; ++hand)
		{
			const ControllerInput & c = fd.input.hand[hand];
			auto bit = [](uint32_t v, uint32_t b) { return (v & b) ? 1.f : 0.f; };
			std::array<float, svr::count> v{};
			v[svr::trigger_value] = c.trigger / 255.f;
			v[svr::trigger_click] = bit(c.buttons, button_trigger);
			v[svr::trigger_touch] = bit(c.touches, button_trigger);
			v[svr::squeeze_value] = c.grip / 255.f;
			v[svr::squeeze_click] = bit(c.buttons, button_grip);
			v[svr::thumbstick_x] = std::clamp(c.thumb_x / 32767.f, -1.f, 1.f);
			v[svr::thumbstick_y] = std::clamp(c.thumb_y / 32767.f, -1.f, 1.f);
			v[svr::thumbstick_click] = bit(c.buttons, button_thumb);
			v[svr::thumbstick_touch] = bit(c.touches, button_thumb);
			v[svr::a_click] = bit(c.buttons, hand ? button_a : button_x);
			v[svr::a_touch] = bit(c.touches, hand ? button_a : button_x);
			v[svr::b_click] = bit(c.buttons, hand ? button_b : button_y);
			v[svr::b_touch] = bit(c.touches, hand ? button_b : button_y);
			v[hand ? svr::system_click : svr::menu_click] = bit(c.buttons, hand ? button_back : button_enter);
			v[svr::thumbrest_touch] = bit(c.touches, button_thumbrest);
			for (int i = 0; i < svr::count; ++i)
			{
				uint32_t id;
				if (v[i] == last_input[hand][i] or not vrl_input_to_wivrn(hand, i, id))
					continue;
				last_input[hand][i] = v[i];
				changed.push_back({id, v[i], t});
			}
		}
		if (not changed.empty())
		{
			stats.inputs += changed.size();
			if (listener)
				listener->on_input(changed);
		}
	}

	// --- audio channel -----------------------------------------------------------------------------------------
	void audio_loop(std::shared_ptr<Channel> chan)
	{
		try
		{
			while (not quit)
			{
				int32_t n;
				if (not chan->read_exact(&n, 4))
					break;
				if (n < 0 or n > (1 << 20) or n % 2)
					throw std::runtime_error("bad audio packet");
				std::vector<int16_t> pcm(size_t(n) / 2);
				if (n and not chan->read_exact(pcm.data(), size_t(n)))
					break;
				stats.mic_packets++;
				if (mic_on and listener)
					listener->on_microphone(pcm, now_ns());
			}
		}
		catch (std::exception & e)
		{
			stats.bad_messages++;
			log(std::string("vd: audio: ") + e.what());
		}
		channel_closed(audio, chan);
	}
};

VdSession::VdSession(VdConfig cfg) : p(std::make_unique<impl>(std::move(cfg))) {}

VdSession::~VdSession()
{
	stop();
}

void VdSession::start(SessionListener * l)
{
	p->listener = l;
	p->quit = false;
	uint16_t * port[] = {&p->bound.control, &p->bound.data, &p->bound.video, &p->bound.audio};
	uint16_t want[] = {p->cfg.control_port, p->cfg.data_port, p->cfg.video_port, p->cfg.audio_port};
	for (int c = 0; c < channel_count; ++c)
		p->listen_fd[c] = listen_tcp(want[c], *port[c]);
	if (p->cfg.discovery_key)
		p->udp_fd = bind_udp(p->cfg.discovery_port, p->bound.discovery);
	else
		p->log("vd: no discovery key: discovery responder off (the headset cannot find this computer)");
	p->accept_thread = std::thread([this] { p->accept_loop(); });
}

void VdSession::stop()
{
	if (p->quit.exchange(true))
		return;
	if (p->accept_thread.joinable())
		p->accept_thread.join();
	p->drop_session("host stopping");
	std::vector<std::thread> readers;
	{
		std::lock_guard lock(p->threads_mutex);
		readers = std::move(p->readers);
	}
	for (auto & t: readers)
		t.join();
	for (int & fd: p->listen_fd)
		if (fd >= 0)
			close(std::exchange(fd, -1));
	if (p->udp_fd >= 0)
		close(std::exchange(p->udp_fd, -1));
}

bool VdSession::connected() const
{
	return p->is_connected;
}

std::optional<HeadsetInfo> VdSession::headset() const
{
	std::lock_guard lock(p->m);
	return p->info;
}

FrameTiming VdSession::next_frame_timing()
{
	ns period = ns(1e9 / p->refresh.load());
	uint32_t d = p->divisor;
	ns wake;
	uint64_t id;
	{
		std::lock_guard lock(p->pace_mutex);
		ns now = now_ns();
		if (p->next_wake == 0 or now - p->next_wake > d * period)
			p->next_wake = now; // first frame, or the producer fell behind: restart the grid
		wake = p->next_wake;
		p->next_wake += d * period;
		id = ++p->frame_id;
	}
	if (ns now = now_ns(); wake > now)
		std::this_thread::sleep_for(std::chrono::nanoseconds(wake - now));
	return {.wake_up = wake, .predicted_display = wake + p->cfg.latency_ns, .frame_id = id, .display_period = period, .render_divisor = d};
}

bool VdSession::views_at(ns t, std::array<Pose, 2> & pose, std::array<Fov, 2> & fov)
{
	fov = default_fov;
	auto head = p->history[size_t(Device::head)].at(t);
	Pose h = head ? head->pose : Pose{};
	for (int e = 0; e < 2; ++e)
	{
		Pose eye{.position = {(e ? 0.5f : -0.5f) * default_ipd_m, 0, 0}}; // TODO(vd-live:ipd)
		pose[e] = multiply(h, eye);
	}
	return head.has_value();
}

std::optional<TrackedPose> VdSession::pose_at(Device d, ns t)
{
	if (d >= Device::count)
		return std::nullopt;
	return p->history[size_t(d)].at(t);
}

void VdSession::submit(const Frame & f)
{
	auto h = headset();
	std::shared_ptr<Channel> v;
	bool annexb_out;
	{
		std::lock_guard lock(p->m);
		v = p->ch[video];
		annexb_out = p->annexb_out;
	}
	if (not p->is_connected or not h or not v)
		return;
	std::lock_guard lock(p->encode_mutex);
	uint32_t d = p->divisor;
	for (auto & e: p->encoders)
		if (not e)
			e = std::make_unique<VtEncoder>(EncoderConfig{
			        .codec = h->codecs.front() == Codec::hevc ? EncoderConfig::Codec::hevc : EncoderConfig::Codec::h264,
			        .width = h->eye_width,
			        .height = h->eye_height,
			        .fps = h->preferred_refresh_rate / d,
			        .bitrate_bps = h->bitrate_bps / 2,
			});
	std::array<CVPixelBufferRef, 2> buf{};
	for (int e = 0; e < 2; ++e)
		if (not f.eye[e].surface or CVPixelBufferCreateWithIOSurface(nullptr, f.eye[e].surface, nullptr, &buf[e]) != kCVReturnSuccess)
			buf[e] = nullptr;
	if (not buf[0] or not buf[1])
	{
		for (auto b: buf)
			if (b)
				CVPixelBufferRelease(b);
		return;
	}
	bool idr = p->idr_pending.exchange(false);
	std::array<EncodedFrame, 2> out;
	std::thread right([&] { out[1] = p->encoders[1]->encode(buf[1], f.display_time, idr); });
	out[0] = p->encoders[0]->encode(buf[0], f.display_time, idr);
	right.join();
	for (auto b: buf)
		CVPixelBufferRelease(b);

	ns now = now_ns();
	ns period = ns(1e9 / p->refresh.load());
	FrameMetadata md;
	md.timestamp_us = f.display_time / 1000; // TODO(vd-live:clock): host clock
	md.frame_index = p->wire_index++;
	md.source = Source::vr;
	md.is_sync_frame = out[0].idr and out[1].idr;
	md.frame_time_ms = float(d * period / 1e6);
	md.game_time_ms = p->last_submit ? float((now - p->last_submit) / 1e6) : md.frame_time_ms;
	p->last_submit = now;
	int64_t encode_ns = std::max(out[0].encode_end - out[0].encode_begin, out[1].encode_end - out[1].encode_begin);
	md.encode_time_ms = float(encode_ns / 1e6);
	md.pose.has_valid_pose = md.pose.left_rendered = md.pose.right_rendered = 1;
	for (int e = 0; e < 2; ++e)
	{
		auto & o = f.pose[e].orientation;
		auto & x = f.pose[e].position;
		md.pose.eye[e] = {{o[0], o[1], o[2], o[3]}, {x[0], x[1], x[2]}};
		md.pose.fov[e] = to_fov_port(f.fov[e]);
	}
	{
		std::lock_guard hl(p->haptic_mutex);
		for (int hand = 0; hand < 2; ++hand)
			md.haptic[hand].amplitude = now < p->haptic_until[hand] ? p->haptic_amp[hand] : 0;
	}
	md.render_width = int32_t(h->eye_width);
	md.render_height = int32_t(h->eye_height);

	std::array<std::vector<uint8_t>, 2> bits;
	for (int e = 0; e < 2; ++e)
		bits[e] = annexb_out ? std::move(out[e].annexb) : length_prefixed(out[e].annexb);
	md.data_size = int32_t(bits[0].size());
	std::vector<uint8_t> frame{tag_frame};
	auto mb = bytes_of(md);
	frame.insert(frame.end(), mb.begin(), mb.end());
	frame.insert(frame.end(), bits[0].begin(), bits[0].end());
	SliceHeader sh{.timestamp_us = md.timestamp_us, .stream_index = 1, .data_size = int32_t(bits[1].size())};
	std::vector<uint8_t> slice{tag_slice};
	auto sb = bytes_of(sh);
	slice.insert(slice.end(), sb.begin(), sb.end());
	slice.insert(slice.end(), bits[1].begin(), bits[1].end());
	if (not v->send(frame) or not v->send(slice))
		return;
	p->stats.frames_sent++;
	if (md.is_sync_frame)
		p->stats.idr_sent++;
	p->stats.bytes_sent += frame.size() + slice.size();
	p->stats.last_encode_ns = encode_ns;
}

void VdSession::haptics(Device d, float duration_s, float frequency, float amplitude)
{
	int hand = d == Device::left_grip or d == Device::left_aim ? 0 : d == Device::right_grip or d == Device::right_aim ? 1 : -1;
	if (hand < 0)
		return;
	std::lock_guard lock(p->haptic_mutex);
	p->haptic_amp[hand] = std::clamp(amplitude, 0.f, 1.f);
	// rides the next frame's metadata: hold at least one frame even for very short pulses
	ns hold = std::max(ns(duration_s * 1e9), ns(2e9 / p->refresh.load()));
	p->haptic_until[hand] = now_ns() + hold;
	p->stats.haptics++;
}

void VdSession::request_idr()
{
	p->idr_pending = true;
}

void VdSession::send_audio(std::span<const int16_t> pcm)
{
	std::shared_ptr<Channel> a;
	{
		std::lock_guard lock(p->m);
		a = p->ch[audio];
	}
	if (not a or not p->is_connected)
		return;
	std::vector<uint8_t> msg(4 + pcm.size_bytes());
	int32_t n = int32_t(pcm.size_bytes());
	memcpy(msg.data(), &n, 4);
	memcpy(msg.data() + 4, pcm.data(), pcm.size_bytes());
	if (a->send(msg))
		p->stats.audio_packets++;
}

void VdSession::set_microphone(bool on)
{
	p->mic_on = on; // TODO(vd-live:audio-format): no microphone request message known; the headset always sends
}

VdSession::Ports VdSession::ports() const
{
	return p->bound;
}

uint32_t VdSession::render_divisor() const
{
	return p->divisor;
}

bool VdSession::set_pairing(bool on)
{
	if (not p->cfg.discovery_key)
		return false;
	p->pairing = on;
	return true;
}

bool VdSession::pairing() const
{
	return p->pairing;
}

std::string VdSession::accounts_file() const
{
	return p->accounts.file();
}

uint32_t VdSession::bitrate_bps() const
{
	auto h = headset();
	return h and h->bitrate_bps ? h->bitrate_bps : p->cfg.bitrate_bps;
}

const VdStats & VdSession::stats() const
{
	return p->stats;
}

std::string VdSession::streamer_settings() const
{
	std::lock_guard lock(p->m);
	return p->streamer.to_json();
}

void VdSession::set_streamer_setting(const std::string & key, const std::string & json_value)
{
	std::shared_ptr<Channel> c;
	{
		std::lock_guard lock(p->m);
		p->streamer.set_raw(key, json_value);
		c = p->ch[control];
	}
	if (c)
		c->send(control_message(Msg::streamer_setting_changed, setting_changed_payload(key, json_value)));
}

SessionControl::PairingState VdControl::pairing_state() const
{
	return s.pairing() ? PairingState::pairing : PairingState::waiting;
}

std::vector<SessionControl::Param> VdControl::params() const
{
	auto ports = s.ports();
	return {
	        {"port", int64_t(ports.control)},
	        {"discovery_port", int64_t(ports.discovery)},
	        {"render_divisor", int64_t(s.render_divisor())},
	};
}

std::vector<SessionControl::Peer> VdControl::paired() const
{
	std::vector<Peer> out;
	for (auto & a: AccountStore(s.accounts_file()).list())
		out.push_back({a.name, a.last_connection});
	return out;
}

int VdControl::unpair(const std::string & name)
{
	return AccountStore(s.accounts_file()).remove(name);
}

SessionControl::Counters VdControl::counters() const
{
	auto & st = s.stats();
	return {.frames_sent = st.frames_sent.load(), .idr_sent = st.idr_sent.load(), .bytes_sent = st.bytes_sent.load(), .last_encode_ns = st.last_encode_ns.load()};
}
} // namespace vrhost
