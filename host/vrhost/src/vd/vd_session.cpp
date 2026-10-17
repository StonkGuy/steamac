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
#include <condition_variable>
#include <deque>
#include <fstream>
#include <ifaddrs.h>
#include <map>
#include <mutex>
#include <net/if.h>
#include <net/if_dl.h>
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

// Until the headset reports its own (mobile FovLeft/FovRight): Quest 3-like half-angles, radians.
constexpr std::array<Fov, 2> default_fov{Fov{-0.96f, 0.82f, 0.87f, -0.96f}, Fov{-0.82f, 0.96f, 0.87f, -0.96f}};

FovPort to_fov_port(const Fov & f)
{
	return {std::tan(f.up), std::tan(-f.down), std::tan(-f.left), std::tan(f.right)};
}

std::optional<Fov> from_fov_json(const std::optional<std::string> & raw)
{
	Settings o;
	if (not raw or not o.parse(*raw))
		return std::nullopt;
	auto up = o.number("UpTan"), down = o.number("DownTan"), left = o.number("LeftTan"), right = o.number("RightTan");
	if (not up or not down or not left or not right or *up <= 0 or *down <= 0 or *left <= 0 or *right <= 0)
		return std::nullopt;
	return Fov{.left = -float(std::atan(*left)), .right = float(std::atan(*right)), .up = float(std::atan(*up)),
	           .down = -float(std::atan(*down))};
}

std::string hmd_name(int32_t t)
{
	switch (HmdType(t))
	{
		case HmdType::quest: return "Oculus Quest";
		case HmdType::quest2: return "Oculus Quest 2";
		case HmdType::quest_pro: return "Meta Quest Pro";
		case HmdType::quest3: return "Meta Quest 3";
		case HmdType::quest3s: return "Meta Quest 3S";
	}
	return "Virtual Desktop headset (type " + std::to_string(t) + ")";
}

bool quest_family(int32_t t)
{
	return t >= int32_t(HmdType::quest) and t <= int32_t(HmdType::quest3s);
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

// Private IPv4 adapters with their link-layer address (the record's PrivateAdapters).
std::vector<NetAdapter> private_adapters(const std::vector<std::string> & only)
{
	std::vector<NetAdapter> out;
	if (not only.empty())
	{
		for (auto & a: only)
			out.push_back({.ip = a});
		return out;
	}
	ifaddrs * list = nullptr;
	if (getifaddrs(&list) != 0)
		return out;
	std::map<std::string, std::string> mac;
	for (auto i = list; i; i = i->ifa_next)
		if (i->ifa_addr and i->ifa_addr->sa_family == AF_LINK)
		{
			auto dl = (sockaddr_dl *)i->ifa_addr;
			if (dl->sdl_alen != 6)
				continue;
			auto b = (const uint8_t *)LLADDR(dl);
			char s[13];
			snprintf(s, sizeof s, "%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
			mac[i->ifa_name] = s;
		}
	for (auto i = list; i; i = i->ifa_next)
		if (i->ifa_addr and i->ifa_addr->sa_family == AF_INET and (i->ifa_flags & IFF_UP) and not(i->ifa_flags & IFF_LOOPBACK))
		{
			char s[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, s, sizeof s);
			out.push_back({.ip = s, .mac = mac[i->ifa_name]});
		}
	freeifaddrs(list);
	return out;
}

// A stable computer id (the record's ID), kept next to the accounts file.
std::string computer_id(const std::filesystem::path & dir)
{
	auto file = dir / "vd_computer_id";
	std::string id;
	if (std::ifstream in(file); in)
		std::getline(in, id);
	if (id.size() == 36)
		return id;
	std::array<uint8_t, 16> g;
	random_bytes(g);
	char s[40];
	snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[0], g[1], g[2], g[3],
	         g[4], g[5], (g[6] & 0x0f) | 0x40, g[7], (g[8] & 0x3f) | 0x80, g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (std::ofstream o(file); o)
		o << s << "\n";
	return s;
}

std::vector<uint8_t> be32(int32_t v)
{
	return {uint8_t(uint32_t(v) >> 24), uint8_t(uint32_t(v) >> 16), uint8_t(uint32_t(v) >> 8), uint8_t(v)};
}
} // namespace

struct VdSession::impl
{
	VdConfig cfg;
	VdStats stats;
	SessionListener * listener = nullptr;
	AccountStore accounts;
	std::string id;
	std::atomic<bool> pairing{false};

	// One "registration" (VD-FACT(record-schema)): channel key + connection id, renewed when a session ends.
	std::mutex reg_mutex;
	AesKey channel_key = AesKey::random();
	std::array<uint8_t, 16> connection_id{};

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
	bool annexb_out = true;
	std::atomic<bool> is_connected{false};
	std::atomic<uint32_t> divisor{1};
	std::atomic<float> refresh{72};
	float ipd = default_ipd_m;
	std::array<Fov, 2> fov = default_fov;

	std::mutex encode_mutex;
	std::array<std::unique_ptr<VtEncoder>, 2> encoders;
	std::atomic<bool> idr_pending{true};
	ns last_submit = 0;

	std::mutex pace_mutex;
	ns next_wake = 0;
	uint64_t frame_id = 0;

	std::array<PoseHistory, size_t(Device::count)> history;
	// VD-FACT(clock): FrameMetadata.frame_index echoes the tracking sample's index the frame was rendered with.
	std::mutex index_mutex;
	int32_t last_tracking_index = 0;
	// Which tracking sample each frame's poses came from. The host pipeline samples the poses right after
	// next_frame_timing (VrPipeline::frame_clock -> pose_at), the test producers through views_at; submit looks the
	// frame up by id and display time. Without this the frame carried the NEWEST index at submit time, so the headset
	// reprojected from a pose newer than the one rendered and the picture followed the head (2026-10-17 live run).
	struct ViewIndex
	{
		uint64_t frame_id = 0;
		ns display = 0;
		int32_t index = 0;
		std::array<TrackedPose, size_t(Device::count)> poses{};
		bool have_poses = false;
	};
	// newest sample per device, as the headset sent it (under index_mutex, together with last_tracking_index)
	std::array<TrackedPose, size_t(Device::count)> latest{};
	bool have_latest = false;
	std::array<ViewIndex, 64> view_index{};
	size_t view_index_pos = 0;
	void remember_view(uint64_t frame_id, ns display)
	{
		std::lock_guard lock(index_mutex);
		view_index[view_index_pos++ % view_index.size()] = {frame_id, display, last_tracking_index, latest, have_latest};
	}
	int32_t rendered_index(uint64_t frame_id, ns display, ns tolerance)
	{
		std::lock_guard lock(index_mutex);
		const ViewIndex * best = nullptr;
		ns best_d = tolerance;
		for (auto & v: view_index)
		{
			if (not v.display)
				continue;
			ns dt = v.display > display ? v.display - display : display - v.display;
			if (frame_id and v.frame_id == frame_id and dt <= tolerance)
				return v.index; // the frame the pacer handed out (the guest echoes its id and display time)
			if (dt < best_d or (dt == best_d and not best))
				best = &v, best_d = dt;
		}
		return best ? best->index : last_tracking_index;
	}
	// VD-FACT(clock) / TODO(vd-live:clock) answered by the 2026-10-17 trial: the headset reprojects each frame from
	// the pose of the tracking sample the frame names, as it sent it. Rendering with a pose the host predicted
	// further (velocity extrapolation to the display time) made the picture stretch and swim on head turns, so the
	// frame for display time t uses the very sample remembered for it, unmodified.
	std::optional<TrackedPose> rendered_pose(Device d, ns t)
	{
		std::lock_guard lock(index_mutex);
		for (size_t k = 0; k < view_index.size(); ++k)
		{
			auto & v = view_index[(view_index_pos + view_index.size() - 1 - k) % view_index.size()];
			if (v.display == t and v.have_poses)
			{
				TrackedPose r = v.poses[size_t(d)];
				r.time = t;
				return r;
			}
		}
		if (not have_latest)
			return std::nullopt;
		TrackedPose r = latest[size_t(d)];
		r.time = t;
		return r;
	}

	// Network side of the video channel: submit() encodes and queues, this thread sends. A headset that stops
	// reading (decoder stall, Wi-Fi drop, its spacewarp/menu transitions) must never hold up the VR link thread that
	// calls submit(): that thread also returns the game's eye images, and a blocked one froze the game.
	struct Outgoing
	{
		std::shared_ptr<Channel> chan;
		std::vector<uint8_t> frame, slice;
		bool idr;
	};
	std::mutex send_mutex;
	std::condition_variable send_cv;
	std::deque<Outgoing> send_queue;
	bool sending = false;
	ns last_drop_log = 0;
	std::thread sender;
	static constexpr size_t send_queue_max = 2; // frames waiting for the network before older ones are dropped
	void sender_loop()
	{
		std::unique_lock lock(send_mutex);
		while (true)
		{
			send_cv.wait(lock, [&] { return quit or not send_queue.empty(); });
			if (send_queue.empty())
				return; // quit
			Outgoing o = std::move(send_queue.front());
			send_queue.pop_front();
			sending = true;
			lock.unlock();
			bool ok = o.chan->send(o.frame) and o.chan->send(o.slice);
			if (ok)
			{
				stats.frames_sent++;
				if (o.idr)
					stats.idr_sent++;
				stats.bytes_sent += o.frame.size() + o.slice.size();
			}
			lock.lock();
			sending = false;
			send_cv.notify_all();
		}
	}
	bool network_behind()
	{
		std::lock_guard lock(send_mutex);
		return not send_queue.empty();
	}
	// false = the headset is behind: the frames waiting (and this one, unless it is an IDR that restarts the stream)
	// were dropped; the caller then asks for an IDR so the next frame decodes without them.
	bool queue_frame(Outgoing o)
	{
		std::lock_guard lock(send_mutex);
		if (send_queue.size() >= send_queue_max)
		{
			stats.frames_dropped += send_queue.size();
			send_queue.clear();
			if (not o.idr)
			{
				stats.frames_dropped++;
				return false;
			}
		}
		send_queue.push_back(std::move(o));
		send_cv.notify_all();
		return true;
	}

	std::mutex haptic_mutex;
	std::array<float, 2> haptic_amp{};
	std::array<ns, 2> haptic_until{};
	std::array<std::array<float, svr::count>, 2> last_input{};
	std::atomic<bool> mic_on{false};

	explicit impl(VdConfig c) :
	        cfg(std::move(c)),
	        accounts(cfg.accounts_file.empty() ? KnownKeys::default_dir() / "vd_accounts" : std::filesystem::path(cfg.accounts_file))
	{
		random_bytes(connection_id);
		cfg.bitrate_bps = std::min(cfg.bitrate_bps, max_bitrate_bps);
		if (not cfg.discovery_key)
			cfg.encrypt_local = false; // no record = no channel key for the headset
		pairing = cfg.discovery_key and (cfg.pairing or accounts.list().empty());
		divisor = cfg.spacewarp == VdConfig::Spacewarp::on ? 2 : 1;
		// VD-FACT(settings-keys): the streamer object we own; members at their defaults may be omitted.
		streamer.set(key_has_vr, true);
		streamer.set_raw(key_streaming_source, std::to_string(int(Source::vr)));
		streamer.set_raw(key_active_codec, std::to_string(int(VideoCodec::hevc)));
		streamer.set(key_allow_extrapolation, true);
		streamer.set(key_auto_bitrate, false);
		streamer.set(key_can_launch_steamvr, false);
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
	ComputerRecord record()
	{
		if (id.empty())
			id = computer_id(accounts.file().parent_path()); // created on first use, next to the accounts
		ComputerRecord r;
		r.id = id;
		r.name = cfg.computer_name.empty() ? host_name() : cfg.computer_name;
		r.description = "steamac";
		r.adapters = private_adapters(cfg.addresses);
		r.encrypt_local = cfg.encrypt_local;
		std::lock_guard lock(reg_mutex);
		r.connection_id = connection_id;
		r.channel_key = channel_key;
		return r;
	}

	void renew_registration()
	{
		std::lock_guard lock(reg_mutex);
		channel_key = AesKey::random();
		random_bytes(connection_id);
	}

	void on_datagram()
	{
		uint8_t buf[65536];
		sockaddr_storage from{};
		socklen_t from_len = sizeof from;
		ssize_t n = recvfrom(udp_fd, buf, sizeof buf, 0, (sockaddr *)&from, &from_len);
		if (n <= 0 or not cfg.discovery_key)
			return;
		std::span<const uint8_t> d(buf, size_t(n));
		if (auto c = parse_connect_now(d))
		{
			// The headset is about to dial the fixed ports; the listeners are up already.
			std::lock_guard lock(reg_mutex);
			if (c->connection_id == connection_id)
				stats.connect_now++;
			return;
		}
		auto offer = unwrap_offer(*cfg.discovery_key, d);
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
		auto reply = encode_reply(offer->rsa_public_xml, record());
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
		// VD-FACT(plain-framing): on the LAN the control channel is always encrypted, the others per the record.
		if (cfg.discovery_key and (c == control or cfg.encrypt_local))
		{
			std::lock_guard lock(reg_mutex);
			chan->encrypt(channel_key);
		}
		// One headset at a time: a new connection replaces the channel's previous one (its reader then ends without
		// touching the session); a closing control channel ends the whole session.
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
			case data:
				// VD-FACT(channel-order): bandwidth probe of 0 bytes (we do not auto-adjust the bitrate).
				chan->send(be32(0));
				spawn([this, chan] { data_loop(chan); });
				break;
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
		{
			log(std::string("vd: session dropped: ") + why);
			renew_registration(); // like the Streamer re-registering: a new channel key and connection id
		}
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
			{
				std::lock_guard lock(index_mutex);
				have_latest = false;
				view_index = {};
			}
			if (listener)
				listener->on_disconnected();
		}
	}

	bool send_control(Msg type, const std::string & payload = {})
	{
		std::shared_ptr<Channel> c;
		{
			std::lock_guard lock(m);
			c = ch[control];
		}
		return c and c->send(control_message(type, payload));
	}

	// --- control channel ---------------------------------------------------------------------------------------
	void control_loop(std::shared_ptr<Channel> chan)
	{
		try
		{
			// VD-FACT(settings-payload): the Streamer publishes its object as soon as the control channel is up.
			{
				std::lock_guard lock(m);
				chan->send(control_message(Msg::reload_streamer_settings, streamer.to_json()));
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
				Settings change;
				if (not change.parse(text))
				{
					stats.bad_messages++;
					return;
				}
				std::lock_guard lock(m);
				Settings & s = type == Msg::user_setting_changed ? user : type == Msg::mobile_setting_changed ? mobile : streamer;
				s.merge(change);
				break;
			}
			case Msg::force_key_frame:
				stats.idr_requests++;
				idr_pending = true;
				return;
			case Msg::restart_stream:
				stats.idr_requests++;
				resend_video_format();
				return;
			case Msg::recenter:
				// The headset re-zeroes its own tracking space; the poses that follow carry it. Nothing to do here.
				stats.recenters++;
				log("vd: recenter");
				return;
			case Msg::update_required:
				log("vd: the headset asks for a newer Streamer version (answered: no update available)");
				send_control(Msg::update_unavailable);
				return;
			case Msg::wake_up_computer:
				send_control(Msg::computer_awaken);
				return;
			case Msg::disconnect:
			case Msg::exit:
				drop_session("headset disconnected");
				return;
			default: // games list, SteamVR, boundary/skeleton data, desktop features: accepted and ignored
				return;
		}
		if (type != Msg::reload_streamer_settings and type != Msg::streamer_setting_changed)
			apply_headset();
	}

	// Headset description from its mobile + user settings (VD-FACT(settings-keys, preset-sizes, fov, ipd, bitrate)).
	void apply_headset()
	{
		HeadsetInfo h;
		uint32_t d;
		int q_logged = -1;
		bool ssw_logged = false;
		float old_refresh = refresh;
		uint32_t old_divisor = divisor;
		uint32_t old_bitrate = 0;
		std::shared_ptr<Channel> v;
		bool format_changed;
		{
			std::lock_guard lock(m);
			if (mobile.empty())
				return; // the headset describes itself in its mobile object
			int32_t hmd = int32_t(mobile.number(key_hmd_type).value_or(-1));
			h.name = hmd_name(hmd);
			int q = std::clamp(int(user.number(key_quality).value_or(int(Quality::medium))), int(Quality::potato),
			                   int(Quality::unstoppable));
			EyeSize stream = quest3_stream[q + 1];
			q_logged = q;
			if (not quest_family(hmd))
				log("vd: headset type " + std::to_string(hmd) + " has no preset table here; using the Quest 3 sizes");
			h.eye_width = cfg.width ? cfg.width : stream.width;
			h.eye_height = cfg.height ? cfg.height : stream.height;
			// The guest renders at the stream size and the headset upscales the decoded image (no host foveation).
			h.render_eye_width = h.eye_width;
			h.render_eye_height = h.eye_height;
			h.foveation = FoveationModel::none;
			h.refresh_rates = {72, 80, 90, 96, 100, 120};
			int rate = int(user.number(key_framerate).value_or(0));
			if (rate == 0)
				rate = quest_family(hmd) ? 72 : 90;
			h.preferred_refresh_rate = std::ranges::contains(h.refresh_rates, float(rate)) ? float(rate) : 72;
			ipd = float(mobile.number(key_ipd).value_or(default_ipd_m));
			fov = {from_fov_json(mobile.raw(key_fov_left)).value_or(default_fov[0]),
			       from_fov_json(mobile.raw(key_fov_right)).value_or(default_fov[1])};
			h.fov = fov;
			auto features = mobile.number(key_features);
			bool hevc = not features or (uint32_t(*features) & feature_hevc);
			h.codecs = cfg.codec == VdConfig::CodecChoice::h264 or (cfg.codec == VdConfig::CodecChoice::automatic and not hevc)
			                   ? std::vector{Codec::h264}
			                   : std::vector{Codec::hevc, Codec::h264};
			h.hand_tracking = features and (uint32_t(*features) & feature_hand_tracking);
			h.eye_gaze = features and (uint32_t(*features) & feature_eye_tracking);
			h.speaker = AudioFormat{speaker_channels, audio_rate};
			h.microphone = AudioFormat{mic_channels, audio_rate};
			h.max_bitrate_bps = max_bitrate_bps;
			bool ssw_headset = mobile.boolean(key_spacewarp).value_or(false);
			ssw_logged = ssw_headset;
			uint32_t bitrate = cfg.bitrate_bps;
			if (auto bw = mobile.number(key_measured_bandwidth); bw and *bw > 0)
				bitrate = std::min(bitrate, uint32_t(std::min(*bw, double(max_bitrate_bps))));
			if (not ssw_headset)
				bitrate /= 2; // VD-FACT(bitrate): the official producer halves the target while SSW is off
			h.bitrate_bps = bitrate;
			annexb_out = mobile.boolean(key_annexb).value_or(true);
			bool ssw = cfg.spacewarp == VdConfig::Spacewarp::on or (cfg.spacewarp == VdConfig::Spacewarp::headset and ssw_headset);
			d = ssw ? 2 : 1;
			old_bitrate = info ? info->bitrate_bps : 0;
			format_changed = info and (info->eye_width != h.eye_width or info->eye_height != h.eye_height or
			                           info->codecs != h.codecs or info->preferred_refresh_rate != h.preferred_refresh_rate);
			info = h;
			v = ch[video];
			std::string codec = std::to_string(int(h.codecs.front() == Codec::hevc ? VideoCodec::hevc : VideoCodec::h264));
			if (streamer.raw(key_active_codec) != codec)
			{
				streamer.set_raw(key_active_codec, codec);
				if (ch[control])
					ch[control]->send(control_message(Msg::streamer_setting_changed, setting_changed_payload(key_active_codec, codec)));
			}
		}
		refresh = h.preferred_refresh_rate;
		divisor = d;
		if (format_changed or old_divisor != d)
		{
			char buf[200];
			snprintf(buf, sizeof buf, "vd: headset '%s' asks: stream %ux%u per eye, %s, %.0f Hz, quality %d, spacewarp %s -> render rate 1/%u",
			         h.name.c_str(), h.eye_width, h.eye_height, h.codecs.front() == Codec::hevc ? "HEVC" : "H.264",
			         h.preferred_refresh_rate, q_logged, ssw_logged ? "on" : "off", d);
			log(buf);
		}
		// the encoders' frame rate and bitrate follow the render divisor (spacewarp) too: new encoders + key frame
		if (format_changed or old_divisor != d or old_bitrate != h.bitrate_bps)
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
		f.frame_rate = int32_t(h.preferred_refresh_rate); // VD-FACT(ssw-cadence): the display rate
		f.frame_size = {int32_t(h.eye_width), int32_t(h.eye_height)};
		f.stream_count = 2;
		f.bounds = {0, 0, int32_t(h.render_eye_width), int32_t(h.render_eye_height)};
		v.send(video_format_message(f));
	}

	void resend_video_format()
	{
		std::optional<HeadsetInfo> h;
		std::shared_ptr<Channel> v;
		{
			std::lock_guard lock(m);
			h = info;
			v = ch[video];
		}
		idr_pending = true;
		if (h and v and is_connected)
			send_video_format(*v, *h);
	}

	// Headset -> host on the video channel is only TCP ACKs; reading tells us when it closes.
	void drain_loop(std::shared_ptr<Channel> chan)
	{
		uint8_t b;
		while (not quit and chan->read_exact(&b, 1))
			;
		channel_closed(video, chan);
	}

	// --- data channel: tracking + input up (VD-FACT(framedata-prefix)) ----------------------------------------------
	void data_loop(std::shared_ptr<Channel> chan)
	{
		try
		{
			std::vector<uint8_t> in, out(frame_data_face);
			while (not quit)
			{
				uint8_t prefix[frame_data_prefix];
				if (not chan->read_exact(prefix, sizeof prefix))
					break;
				int32_t comp;
				uint64_t hash;
				memcpy(&comp, prefix, 4);
				memcpy(&hash, prefix + 4, 8);
				if (comp <= 0 or comp > (1 << 20))
					throw std::runtime_error("bad FrameData size");
				in.resize(size_t(comp));
				if (not chan->read_exact(in.data(), in.size()))
					break;
				int n = LZ4_decompress_safe((const char *)in.data(), (char *)out.data(), comp, int(out.size()));
				if (n <= 0 or XXH3_64bits(out.data(), size_t(n)) != hash)
				{
					stats.bad_messages++;
					continue;
				}
				if (auto s = decode_tracking(std::span(out.data(), size_t(n))))
					on_tracking_sample(*s);
				else
					stats.bad_messages++;
			}
		}
		catch (std::exception & e)
		{
			stats.bad_messages++;
			log(std::string("vd: data: ") + e.what());
		}
		channel_closed(data, chan);
	}

	static TrackedPose tracked(Device d, const Quat & o, const Vec3 & p, const Vec3 & v, const Vec3 & w, bool valid, ns time)
	{
		TrackedPose t{.device = d, .time = time};
		t.pose.orientation = {o.x, o.y, o.z, o.w};
		t.pose.position = {p.x, p.y, p.z};
		t.linear_velocity = {v.x, v.y, v.z};
		t.angular_velocity = {w.x, w.y, w.z};
		// a pose that is not a finite unit quaternion + finite position (a sleeping controller, a corrupt sample) is
		// reported untracked: extrapolating it would hand the game NaN matrices
		float n2 = o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w;
		bool finite = std::isfinite(n2) and std::isfinite(p.x) and std::isfinite(p.y) and std::isfinite(p.z) and
		              std::isfinite(v.x) and std::isfinite(v.y) and std::isfinite(v.z) and std::isfinite(w.x) and
		              std::isfinite(w.y) and std::isfinite(w.z);
		if (not finite or n2 < 0.25f or n2 > 4.f)
		{
			t.pose = Pose{};
			t.linear_velocity = t.angular_velocity = {};
			valid = false;
		}
		t.orientation_valid = t.position_valid = valid;
		return t;
	}

	void on_tracking_sample(const TrackingSample & s)
	{
		ns t = now_ns(); // VD-FACT(clock): no clock sync; the sample time is the arrival time
		stats.last_latency_us = int32_t(s.latency_s * 1e6f);
		auto & hd = s.head;
		auto ctl = [&](Device d, int i) {
			auto & c = s.controller[i];
			return tracked(d, c.orientation, c.position, c.velocity, c.angular_velocity, c.tracked != 0, t);
		};
		std::array<TrackedPose, 5> poses{
		        tracked(Device::head, hd.orientation, hd.position, hd.velocity, hd.angular_velocity, hd.tracked, t),
		        ctl(Device::left_grip, 0),
		        ctl(Device::left_aim, 0), // VD-FACT(aim): controllers carry one pose
		        ctl(Device::right_grip, 1),
		        ctl(Device::right_aim, 1),
		};
		for (auto & p: poses)
			history[size_t(p.device)].add(p);
		{
			// the sample's index and its poses change together: a frame names exactly the sample it rendered
			std::lock_guard lock(index_mutex);
			last_tracking_index = s.frame_index;
			for (auto & p: poses)
				latest[size_t(p.device)] = p;
			have_latest = true;
		}
		stats.tracking++;
		if (listener)
			listener->on_tracking(poses);

		// VD-FACT(button-bits): one bitfield for both controllers.
		std::vector<InputValue> changed;
		const ControllerState & c = s.input;
		auto bit = [](uint16_t v, uint16_t b) { return (v & b) ? 1.f : 0.f; };
		for (int hand = 0; hand < 2; ++hand)
		{
			bool r = hand == 1;
			std::array<float, svr::count> v{};
			v[svr::trigger_value] = c.trigger[hand] / 255.f;
			v[svr::trigger_click] = bit(c.buttons, r ? button_right_trigger : button_left_trigger);
			v[svr::trigger_touch] = bit(c.touches, r ? touch_right_trigger : touch_left_trigger);
			v[svr::squeeze_value] = c.grip[hand] / 255.f;
			v[svr::squeeze_click] = bit(c.buttons, r ? button_right_grip : button_left_grip);
			v[svr::thumbstick_x] = std::clamp(c.thumb_x[hand] / 32767.f, -1.f, 1.f);
			v[svr::thumbstick_y] = std::clamp(c.thumb_y[hand] / 32767.f, -1.f, 1.f);
			v[svr::thumbstick_click] = bit(c.buttons, r ? button_right_thumb : button_left_thumb);
			v[svr::thumbstick_touch] = bit(c.touches, r ? touch_right_thumb : touch_left_thumb);
			v[svr::a_click] = bit(c.buttons, r ? button_a : button_x);
			v[svr::a_touch] = bit(c.touches, r ? touch_a : touch_x);
			v[svr::b_click] = bit(c.buttons, r ? button_b : button_y);
			v[svr::b_touch] = bit(c.touches, r ? touch_b : touch_y);
			v[r ? svr::system_click : svr::menu_click] = bit(c.buttons, r ? button_back : button_enter);
			v[svr::thumbrest_touch] = bit(c.touches, r ? touch_right_thumbrest : touch_left_thumbrest);
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

	// --- audio channel (VD-FACT(audio-format)) ------------------------------------------------------------------
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
	p->sender = std::thread([this] { p->sender_loop(); });
}

void VdSession::stop()
{
	if (p->quit.exchange(true))
		return;
	if (p->accept_thread.joinable())
		p->accept_thread.join();
	p->drop_session("host stopping");
	{
		std::lock_guard lock(p->send_mutex);
		p->send_queue.clear();
		p->send_cv.notify_all();
	}
	if (p->sender.joinable())
		p->sender.join();
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
	// the caller samples the poses for this frame now (VrPipeline::frame_clock): remember the tracking sample
	p->remember_view(id, wake + p->cfg.latency_ns);
	return {.wake_up = wake, .predicted_display = wake + p->cfg.latency_ns, .frame_id = id, .display_period = period, .render_divisor = d};
}

bool VdSession::views_at(ns t, std::array<Pose, 2> & pose, std::array<Fov, 2> & fov)
{
	float ipd;
	{
		std::lock_guard lock(p->m);
		fov = p->fov;
		ipd = p->ipd;
	}
	p->remember_view(0, t); // which tracking sample this frame's views come from (FrameMetadata.frame_index)
	auto head = p->rendered_pose(Device::head, t);
	Pose h = head ? head->pose : Pose{};
	for (int e = 0; e < 2; ++e)
	{
		Pose eye{.position = {(e ? 0.5f : -0.5f) * ipd, 0, 0}};
		pose[e] = multiply(h, eye);
	}
	return head.has_value();
}

std::optional<TrackedPose> VdSession::pose_at(Device d, ns t)
{
	if (d >= Device::count)
		return std::nullopt;
	return p->rendered_pose(d, t);
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
	// The headset is a whole frame behind (one frame waiting behind the one being sent): skip this frame BEFORE
	// encoding it. The encoder never sees it, so the stream stays decodable without a key frame; dropping encoded
	// frames instead forces an IDR, and the large IDR frames made a slow link fall further behind (2026-10-17: 144-255
	// drops per session, each with a key frame).
	if (p->network_behind())
	{
		p->stats.frames_dropped++;
		if (ns now = now_ns(); now - p->last_drop_log > 1'000'000'000)
		{
			p->last_drop_log = now;
			p->log("vd: the headset is not reading the video channel fast enough: frames skipped (" +
			       std::to_string(p->stats.frames_dropped.load()) + " so far)");
		}
		return;
	}
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
	md.timestamp_us = now / 1000; // VD-FACT(clock): host microseconds, pairs the frame with its slice
	md.frame_index = p->rendered_index(f.frame_id, f.display_time, ns(d * period));
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
	md.render_width = int32_t(h->render_eye_width);
	md.render_height = int32_t(h->render_eye_height);

	// VD-FACT(eye-delivery, annexb-default): eye 0 in the frame message, eye 1 as slice 1, same timestamp.
	std::array<std::vector<uint8_t>, 2> bits;
	for (int e = 0; e < 2; ++e)
		bits[e] = annexb_out ? std::move(out[e].annexb) : length_prefixed(out[e].annexb);
	md.data_size = int32_t(bits[0].size());
	auto frame = frame_message(md, bits[0]);
	auto slice = slice_message({.timestamp_us = md.timestamp_us, .stream_index = 1, .data_size = int32_t(bits[1].size())}, bits[1]);
	p->stats.last_encode_ns = encode_ns;
	if (not p->queue_frame({std::move(v), std::move(frame), std::move(slice), md.is_sync_frame != 0}))
	{
		p->idr_pending = true; // the headset fell behind: restart the stream with a key frame
		if (now - p->last_drop_log > 1'000'000'000)
		{
			p->last_drop_log = now;
			p->log("vd: the headset is not reading the video channel fast enough: frames dropped (" +
			       std::to_string(p->stats.frames_dropped.load()) + " so far), key frame next");
		}
	}
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
	// VD-FACT(audio-format): LE i32 byte count + S16 PCM, in packets of at most 1152 bytes like the Streamer's.
	auto bytes = std::span(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size_bytes());
	while (not bytes.empty())
	{
		auto part = bytes.first(std::min<size_t>(bytes.size(), speaker_packet_bytes));
		bytes = bytes.subspan(part.size());
		std::vector<uint8_t> msg(4 + part.size());
		int32_t n = int32_t(part.size());
		memcpy(msg.data(), &n, 4);
		memcpy(msg.data() + 4, part.data(), part.size());
		if (not a->send(msg))
			return;
		p->stats.audio_packets++;
	}
}

void VdSession::set_microphone(bool on)
{
	// VD-FACT(audio-format): the headset sends its microphone per its own MicPassthrough setting; this only gates
	// delivery to the listener.
	p->mic_on = on;
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

bool VdSession::send_command(Msg m)
{
	switch (m)
	{
		case Msg::enable_vr_passthrough:
		case Msg::disable_vr_passthrough:
		case Msg::toggle_vr_passthrough:
		case Msg::toggle_hand_passthrough:
		case Msg::toggle_desk_passthrough:
		case Msg::toggle_performance_overlay:
		case Msg::toggle_foveated_streaming:
			break;
		default:
			return false;
	}
	if (not p->is_connected or not p->send_control(m))
		return false;
	p->stats.commands_sent++;
	return true;
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
