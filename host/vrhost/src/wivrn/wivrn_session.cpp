// SPDX-License-Identifier: GPL-3.0-or-later
// Parts adapted from WiVRn v26.9 (GPL-3.0-or-later): server/driver/wivrn_connection.cpp (handshake sequence),
// server/driver/clock_offset.cpp (clock regression), server/encoder/idr_handler.cpp (IDR state machine),
// server/encoder/video_encoder.cpp (SendData sharding and timing). The crypto handshake, PIN check (SMP) and
// AES-CTR socket encryption use WiVRn's own common/ code unmodified; the pacer is WiVRn's server/compositor/pacer.cpp
// unmodified.
#include "wivrn/wivrn_session.h"

#include "encoder/vt_encoder.h"
#include "session/clock.h"
#include "wivrn/dnssd_publisher.h"
#include "wivrn/known_keys.h"

#include "compositor/pacer.h"
#include "crypto.h"
#include "driver/clock_offset.h"
#include "secrets.h"
#include "smp.h"

#include "protocol_version.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <CoreVideo/CoreVideo.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <poll.h>
#include <random>
#include <unistd.h>

using namespace std::chrono_literals;

namespace vrhost
{
using control_socket = wivrn::typed_socket<wivrn::TCP, wivrn::from_headset::packets, wivrn::to_headset::packets>;
using stream_socket = wivrn::typed_socket<wivrn::UDP, wivrn::from_headset::packets, wivrn::to_headset::packets>;
namespace fh = wivrn::from_headset;
namespace th = wivrn::to_headset;

// ---- clock offset: host = x, headset = y = x + b (WiVRn clock_offset_estimator, host clock instead of Monado's) ----
class ClockOffset
{
	struct sample
	{
		int64_t query, response, received;
	};
	static constexpr size_t num_samples = 100;
	std::mutex mutex;
	std::vector<sample> samples;
	size_t index = 0;
	std::atomic<int64_t> b{0};
	std::atomic<int> interval_ms{10};

public:
	int interval() const { return interval_ms; }
	void reset()
	{
		std::lock_guard lock(mutex);
		samples.clear();
		index = 0;
		b = 0;
		interval_ms = 10;
	}
	void add(int64_t query, int64_t response)
	{
		int64_t now = now_ns();
		std::lock_guard lock(mutex);
		sample s{query, response, now};
		if (samples.size() < num_samples)
			samples.push_back(s);
		else
		{
			interval_ms = 100;
			int64_t latency = 0;
			for (auto & x: samples)
				latency += x.received - x.query;
			latency /= int64_t(samples.size());
			if (s.received - s.query > 3 * latency)
				return; // likely retransmitted / delayed
			samples[index] = s;
			index = (index + 1) % num_samples;
		}
		double x0 = 0, y0 = 0;
		for (auto & x: samples)
		{
			x0 += (x.query + x.received) * 0.5;
			y0 += x.response;
		}
		x0 /= samples.size();
		y0 /= samples.size();
		b = int64_t(y0 - x0);
	}
	bool stable() const
	{
		return b != 0;
	}
	int64_t to_headset(int64_t t) const { return t + b; }
	wivrn::clock_offset offset() const { return wivrn::clock_offset{.b = b, .stable = stable()}; }
	int64_t from_headset(int64_t t) const { return t - b; }
};

// ---- IDR state machine (WiVRn default_idr_handler without non-reference frames) ----
class IdrState
{
	enum
	{
		need_idr,
		wait_feedback,
		idr_received,
		running
	} state = need_idr;
	uint64_t ref = 0;
	std::mutex mutex;

public:
	void reset()
	{
		std::lock_guard l(mutex);
		state = need_idr;
	}
	bool request()
	{
		std::lock_guard l(mutex);
		bool was = state != need_idr;
		state = need_idr;
		return was;
	}
	// Returns true when this frame must be an IDR.
	bool frame_is_idr(uint64_t frame)
	{
		std::lock_guard l(mutex);
		switch (state)
		{
			case need_idr:
				state = wait_feedback;
				ref = frame;
				return true;
			case wait_feedback:
				if (frame > ref + 100) // feedback for the IDR never came
				{
					state = wait_feedback;
					ref = frame;
					return true;
				}
				return false;
			case idr_received:
				state = running;
				ref = frame;
				return false;
			case running:
				return false;
		}
		return false;
	}
	// Returns true if an IDR became necessary.
	bool on_feedback(uint64_t frame, bool sent_to_decoder)
	{
		std::lock_guard l(mutex);
		if (state == wait_feedback and frame == ref)
		{
			state = sent_to_decoder ? idr_received : need_idr;
			return not sent_to_decoder;
		}
		if (state == running and not sent_to_decoder and frame >= ref)
		{
			state = need_idr;
			return true;
		}
		return false;
	}
};

// PIN guessing limit (impl::pin_failed): the first failure is free (a typo), then 2, 4, ... 60 s between attempts.
constexpr int pin_max_failures = 10;
static std::chrono::seconds pin_backoff(int failures)
{
	if (failures <= 1)
		return std::chrono::seconds(0);
	return std::chrono::seconds(std::min(60, 1 << std::min(failures - 1, 6)));
}

// A refresh rate from the headset (headset_info, refresh_rate_changed): the pacer divides by it and converts the
// period to an integer, so 0, negative, NaN or a denormal would be undefined behaviour or a zero frame duration.
static bool valid_refresh_rate(float hz)
{
	return std::isfinite(hz) and hz >= 1 and hz <= 1000;
}

// An audio format from the headset: the bridge resamples the game audio to it and cuts packets of rate * 10 ms, so a
// rate below 100 Hz made empty packets (an endless loop) and a huge rate or channel count a multi-GiB buffer.
static bool valid_audio(const fh::headset_info_packet::audio_description & a)
{
	return a.num_channels >= 1 and a.num_channels <= 8 and a.sample_rate >= 8000 and a.sample_rate <= 192000;
}

static Device to_device(wivrn::device_id id, bool & ok)
{
	ok = true;
	switch (id)
	{
		case wivrn::device_id::HEAD:
			return Device::head;
		case wivrn::device_id::LEFT_GRIP:
			return Device::left_grip;
		case wivrn::device_id::LEFT_AIM:
			return Device::left_aim;
		case wivrn::device_id::RIGHT_GRIP:
			return Device::right_grip;
		case wivrn::device_id::RIGHT_AIM:
			return Device::right_aim;
		default:
			ok = false;
			return Device::count;
	}
}

static wivrn::device_id from_device(Device d)
{
	switch (d)
	{
		case Device::left_grip:
		case Device::left_aim:
			return wivrn::device_id::LEFT_CONTROLLER_HAPTIC;
		case Device::right_grip:
		case Device::right_aim:
			return wivrn::device_id::RIGHT_CONTROLLER_HAPTIC;
		default:
			return wivrn::device_id::HEAD;
	}
}

static XrPosef to_xr(const Pose & p)
{
	return XrPosef{
	        .orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]},
	        .position = {p.position[0], p.position[1], p.position[2]},
	};
}

static Pose from_xr(const XrPosef & p)
{
	return Pose{
	        .orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
	        .position = {p.position.x, p.position.y, p.position.z},
	};
}

static Fov from_xr(const XrFovf & f)
{
	return Fov{f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
}

static XrFovf to_xr(const Fov & f)
{
	return XrFovf{f.left, f.right, f.up, f.down};
}

struct WivrnSession::impl
{
	WivrnConfig cfg;
	SessionListener * listener = nullptr;
	WivrnStats stats;

	std::unique_ptr<wivrn::TCPListener> tcp_listener;
	int bound_port = 0;
	std::unique_ptr<DnssdPublisher> publisher;
	std::string cookie;

	std::atomic<bool> quit{false};
	std::thread accept_thread;

	// connection state (valid while connected)
	mutable std::mutex cnx_mutex;
	std::unique_ptr<control_socket> control;
	std::unique_ptr<stream_socket> stream;
	std::optional<HeadsetInfo> info;
	std::atomic<bool> is_connected{false};
	std::atomic<float> refresh{72};
	std::atomic<bool> mic_wanted{false}; // set_microphone(); re-sent on every connection

	ClockOffset clock;
	std::array<PoseHistory, size_t(Device::count)> history;
	std::mutex view_mutex;
	std::array<Pose, 2> view_pose{};  // eye relative to head
	std::array<Fov, 2> view_fov{};

	// encoder state
	mutable std::mutex encode_mutex;          // encoder (re)creation
	std::array<std::mutex, 2> eye_mutex; // per-eye encode
	std::array<std::unique_ptr<VtEncoder>, 2> encoders;
	std::array<IdrState, 2> idr;
	EncoderConfig::Codec codec = EncoderConfig::Codec::hevc;
	std::array<ns, 2> last_idr_ns{}; // keyframe_interval_s (under eye_mutex)

	// pacing: WiVRn's pacer, fed by the headset's feedback packets
	wivrn::pacer pacer{1'000'000'000 / 72};

	// Video frame index on the wire. The headset expects consecutive indices: a skipped one is reported as a lost
	// frame ("not sent because no shard was received") and answered with an IDR. The pacer's frame ids are not
	// consecutive on the wire when the producer asks for a frame timing and then submits nothing for it (a game that
	// misses a frame), so sent frames are numbered separately, starting at the first frame's id, and the headset's
	// feedback is mapped back to the pacer's id.
	struct WireIndex
	{
		std::mutex mutex;
		bool started = false;
		uint64_t next = 0;
		std::array<std::pair<uint64_t, uint64_t>, 256> sent{}; // wire index -> pacer frame id

		void reset()
		{
			std::lock_guard l(mutex);
			started = false;
		}
		uint64_t assign(uint64_t frame_id)
		{
			std::lock_guard l(mutex);
			if (not started)
				next = frame_id;
			started = true;
			sent[next % sent.size()] = {next, frame_id};
			return next++;
		}
		std::optional<uint64_t> frame_id(uint64_t wire)
		{
			std::lock_guard l(mutex);
			auto [w, id] = sent[wire % sent.size()];
			if (not started or w != wire)
				return std::nullopt;
			return id;
		}
	} wire_index;

	// encryption / pairing
	std::unique_ptr<KnownKeys> keys;
	mutable std::mutex crypto_mutex; // cfg.encryption + pin: WivrnSession::set_pairing() changes them at run time
	std::string pin;
	std::string advertised_name;
	std::optional<PairingRequest> pairing_request; // under crypto_mutex
	// Online PIN guessing (WiVRn has no limit; the PIN is 6 digits): after a failed PIN check the next one waits
	// pin_backoff(failures), and a random PIN is replaced after pin_max_failures (under crypto_mutex).
	int pin_failures = 0;
	std::chrono::steady_clock::time_point pin_retry_after{};

	// headset state for the control socket (cnx_mutex)
	std::optional<Battery> battery;
	AppRequest app_request;

	void log(const std::string & s)
	{
		if (cfg.log)
			cfg.log(s);
	}

	void pin_failed()
	{
		std::string new_pin;
		{
			std::lock_guard lock(crypto_mutex);
			++pin_failures;
			pin_retry_after = std::chrono::steady_clock::now() + pin_backoff(pin_failures);
			if (pin_failures >= pin_max_failures and cfg.pin.empty() and not pin.empty())
			{
				pin = KnownKeys::random_pin(); // the guesses so far say nothing about the new one
				new_pin = pin;
				pin_failures = 0;
			}
		}
		if (not new_pin.empty())
		{
			log("too many incorrect PINs: new pairing PIN");
			if (cfg.on_pin)
				cfg.on_pin(new_pin);
		}
	}

	void accept_loop();
	void run_connection(wivrn::TCP && tcp);
	void handshake(control_socket & ctl, stream_socket & strm, fh::headset_info_packet & out);
	void on_headset_info(const fh::headset_info_packet &);
	void handle(fh::packets && packet);
	void send_frame_eye(int eye, const Frame & f, uint64_t wire, CVPixelBufferRef buf);
};

// The sender of a datagram on the dual-stack (AF_INET6) stream socket as a sockaddr_in6. macOS reports an IPv4 sender
// as a sockaddr_in (Linux: a v4-mapped sockaddr_in6, which WiVRn's server compares with the control socket's
// v4-mapped peer); map it the same way, or every datagram of an IPv4 headset looks foreign.
static sockaddr_in6 stream_sender(const sockaddr_storage & from)
{
	if (from.ss_family != AF_INET)
		return reinterpret_cast<const sockaddr_in6 &>(from);
	auto & v4 = reinterpret_cast<const sockaddr_in &>(from);
	sockaddr_in6 v6{};
	v6.sin6_family = AF_INET6;
	v6.sin6_port = v4.sin_port;
	v6.sin6_addr.s6_addr[10] = v6.sin6_addr.s6_addr[11] = 0xff;
	memcpy(&v6.sin6_addr.s6_addr[12], &v4.sin_addr, 4);
	return v6;
}

// "from_headset::pin_check_1" for the log
template <typename T>
static std::string packet_name()
{
	std::string f = __PRETTY_FUNCTION__; // "... [T = wivrn::from_headset::pin_check_1]"
	auto b = f.find("T = "), e = f.rfind(']');
	if (b == std::string::npos or e == std::string::npos or e < b)
		return "packet";
	f = f.substr(b + 4, e - b - 4);
	if (f.starts_with("wivrn::"))
		f.erase(0, 7);
	return f;
}

// Throwing receive helper for the handshake phase.
template <typename T>
static T expect(control_socket & ctl, stream_socket * strm, std::chrono::milliseconds timeout, int * udp_port = nullptr,
                const sockaddr_in6 * peer = nullptr)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (auto pending = ctl.receive_pending())
		{
			if (auto * v = std::get_if<T>(&*pending))
				return std::move(*v);
			throw std::runtime_error("unexpected packet during handshake (index " + std::to_string(pending->index()) + ")");
		}
		pollfd fds[2] = {{.fd = ctl.get_fd(), .events = POLLIN}, {.fd = strm ? strm->get_fd() : -1, .events = POLLIN}};
		int r = ::poll(fds, strm ? 2 : 1, 100);
		if (r < 0)
			throw std::system_error(errno, std::system_category());
		// POLLHUP comes with the peer's FIN while its last packets are still unread: read them first
		if ((fds[0].revents & (POLLHUP | POLLERR)) and not(fds[0].revents & POLLIN))
			throw std::runtime_error("control socket closed during handshake");
		if (strm and (fds[1].revents & POLLIN))
		{
			// Not UDP::receive_from_raw(): it sizes the buffer with recvfrom(MSG_PEEK | MSG_TRUNC), which returns the
			// datagram length on Linux but 0 on macOS. Peek the sender address, then let the socket read and decrypt
			// the datagram (UDP::receive_raw, AES-CTR when encryption is on).
			sockaddr_storage from_any{};
			socklen_t from_len = sizeof(from_any);
			uint8_t peek;
			if (recvfrom(strm->get_fd(), &peek, 1, MSG_PEEK, (sockaddr *)&from_any, &from_len) < 0)
				continue;
			sockaddr_in6 from = stream_sender(from_any);
			if (peer and memcmp(&from.sin6_addr, &peer->sin6_addr, sizeof(from.sin6_addr)) != 0)
			{
				recv(strm->get_fd(), &peek, 1, 0); // wrong sender, like wivrn_connection: drop it
				continue;
			}
			std::optional<fh::packets> pkt;
			try
			{
				pkt = strm->receive();
			}
			catch (std::exception &)
			{
				continue; // undecodable (e.g. wrong key): ignore, like a lost datagram
			}
			if (not pkt)
				continue;
			if (auto * v = std::get_if<T>(&*pkt))
			{
				if (udp_port)
					*udp_port = ntohs(from.sin6_port);
				return std::move(*v);
			}
		}
		if (fds[0].revents & POLLIN)
		{
			auto pkt = ctl.receive();
			if (not pkt)
				continue;
			if (auto * v = std::get_if<T>(&*pkt))
				return std::move(*v);
			throw std::runtime_error("unexpected packet during handshake (index " + std::to_string(pkt->index()) + ")");
		}
	}
	throw std::runtime_error("handshake timeout (waiting for " + packet_name<T>() + ")");
}

void WivrnSession::impl::handshake(control_socket & ctl, stream_socket & strm, fh::headset_info_packet & out)
{
	sockaddr_in6 server_address{}, client_address{};
	socklen_t len = sizeof(server_address);
	getsockname(ctl.get_fd(), (sockaddr *)&server_address, &len);
	len = sizeof(client_address);
	getpeername(ctl.get_fd(), (sockaddr *)&client_address, &len);

	int port = ntohs(server_address.sin6_port);
	if (cfg.tcp_only)
		port = -1;
	else
		strm.bind(server_address);

	auto hs = expect<fh::crypto_handshake>(ctl, nullptr, 10s);
	if (hs.protocol_version != wivrn::protocol_version)
	{
		ctl.send(th::crypto_handshake{.state = th::crypto_handshake::crypto_state::incompatible_version});
		char buf[96];
		snprintf(buf, sizeof buf, "incompatible protocol %016" PRIx64 " (ours %016" PRIx64 ")", hs.protocol_version, wivrn::protocol_version);
		throw std::runtime_error(buf);
	}
	log("headset '" + hs.name + "' connected, protocol ok");

	// Crypto handshake: WiVRn v26.9 wivrn_connection::init(), same states and order.
	using cs = th::crypto_handshake::crypto_state;
	using Enc = WivrnConfig::Encryption;
	std::string headset_public_key = KnownKeys::clean_key(hs.public_key);
	Enc encryption;
	std::string pin;
	{
		std::lock_guard lock(crypto_mutex); // one snapshot per connection (set_pairing may change them)
		encryption = cfg.encryption;
		pin = this->pin;
	}
	bool is_public_key_known = encryption != Enc::disabled and keys->contains(headset_public_key);
	if (not is_public_key_known and encryption != Enc::disabled)
	{
		std::string new_pin;
		{
			std::lock_guard lock(crypto_mutex);
			bool open = encryption == Enc::enabled and cfg.pair_on_request;
			if (open and cfg.encryption == Enc::enabled) // still off (the control socket may have opened it meanwhile)
			{
				this->pin = cfg.pin.empty() ? KnownKeys::random_pin() : cfg.pin;
				cfg.encryption = Enc::pairing;
				new_pin = this->pin;
			}
			if (open)
			{
				encryption = Enc::pairing;
				pin = this->pin;
			}
			pairing_request = PairingRequest{hs.name, int64_t(time(nullptr)), encryption == Enc::enabled};
		}
		if (not new_pin.empty())
		{
			log("headset '" + hs.name + "' is not paired: pairing opened on request");
			if (cfg.on_pin)
				cfg.on_pin(new_pin);
		}
	}
	switch (encryption)
	{
		case Enc::disabled:
			ctl.send(th::crypto_handshake{.state = cs::encryption_disabled});
			break;

		case Enc::enabled:
			if (not is_public_key_known)
			{
				ctl.send(th::crypto_handshake{.state = cs::pairing_disabled});
				throw std::runtime_error("headset '" + hs.name + "' is not paired and pairing is disabled");
			}
			[[fallthrough]];

		case Enc::pairing: {
			crypto::key headset_key = crypto::key::from_public_key(hs.public_key);
			// Ephemeral key pair just for exchanging the AES key
			crypto::key server_key = crypto::key::generate_x448_keypair();
			ctl.send(th::crypto_handshake{
			        .public_key = server_key.public_key(),
			        .state = is_public_key_known ? cs::client_already_paired : cs::pin_needed,
			});
			if (not is_public_key_known)
			{
				{
					std::lock_guard lock(crypto_mutex);
					if (std::chrono::steady_clock::now() < pin_retry_after)
						throw std::runtime_error("headset '" + hs.name + "': PIN attempt refused (" +
						                         std::to_string(pin_failures) + " failed, retry later)");
				}
				log("headset '" + hs.name + "' is not paired: waiting for the PIN");
				bool pin_ok = false;
				try
				{
					crypto::smp pin_check;
					auto msg1 = expect<fh::pin_check_1>(ctl, nullptr, 2min).message;
					auto msg2 = pin_check.step2(msg1, pin);
					ctl.send(th::pin_check_2{msg2});
					auto msg3 = expect<fh::pin_check_3>(ctl, nullptr, 10s).message;
					auto [msg4, pin_match] = pin_check.step4(msg3);
					ctl.send(th::pin_check_4{msg4});
					pin_ok = pin_match;
				}
				catch (crypto::smp_cheated &)
				{
				}
				if (not pin_ok)
				{
					pin_failed();
					throw std::runtime_error("incorrect PIN from headset '" + hs.name + "'");
				}
				{
					std::lock_guard lock(crypto_mutex);
					pin_failures = 0;
				}
				log("headset '" + hs.name + "': PIN ok");
			}
			secrets s{server_key, headset_key, is_public_key_known ? "000000" : pin};
			ctl.set_aes_key_and_ivs(s.control_key, s.control_iv_from_headset, s.control_iv_to_headset);
			strm.set_aes_key_and_ivs(s.stream_key, s.stream_iv_header_from_headset, s.stream_iv_header_to_headset);
			break;
		}
	}
	expect<fh::crypto_handshake>(ctl, nullptr, 10s); // client confirms its encryption setup

	ctl.send(th::handshake{.stream_port = port});
	int client_port = -1;
	if (port >= 0)
	{
		expect<fh::handshake>(ctl, &strm, 10s, &client_port, &client_address);
		if (client_port >= 0)
		{
			client_address.sin6_port = htons(client_port);
			strm.connect(client_address);
			strm.set_send_buffer_size(1024 * 1024 * 5);
		}
	}
	else
		expect<fh::handshake>(ctl, nullptr, 10s);
	ctl.send(th::handshake{.stream_port = port});
	try
	{
		out = expect<fh::headset_info_packet>(ctl, nullptr, 10s);
	}
	catch (std::exception & e)
	{
		throw std::runtime_error(std::string("headset info: ") + e.what());
	}

	if (encryption == Enc::pairing and not is_public_key_known)
	{
		keys->add({.public_key = headset_public_key, .name = hs.name});
		stats.paired++;
		log("headset '" + hs.name + "' paired (" + keys->file().string() + ")");
		bool close_pairing = false;
		{
			std::lock_guard lock(crypto_mutex);
			pairing_request.reset();
			// pairing opened for this headset (pair_on_request): closed again once it has paired, as WiVRn's
			// dashboard does after a successful pairing
			close_pairing = cfg.pair_on_request and cfg.encryption == Enc::pairing;
			if (close_pairing)
			{
				cfg.encryption = Enc::enabled;
				this->pin.clear();
			}
		}
		if (close_pairing and cfg.on_pin)
			cfg.on_pin("");
	}
	else if (encryption != Enc::disabled)
		keys->touch(headset_public_key);
	log(encryption == Enc::disabled ? "encryption disabled" : "encryption on (AES-128-CTR)");
}

void WivrnSession::impl::on_headset_info(const fh::headset_info_packet & hi)
{
	HeadsetInfo h;
	h.nominal_render_width = hi.render_eye_width;
	h.nominal_render_height = hi.render_eye_height;
	auto scaled = [&](uint32_t v) { return std::clamp(uint32_t(std::lround(v * cfg.render_scale)) & ~1u, 256u, 8192u); };
	h.render_eye_width = cfg.render_scale == 1 or not hi.render_eye_width ? hi.render_eye_width : scaled(hi.render_eye_width);
	h.render_eye_height = cfg.render_scale == 1 or not hi.render_eye_height ? hi.render_eye_height : scaled(hi.render_eye_height);
	h.eye_width = (cfg.width ? cfg.width : hi.stream_eye_width) & ~1u;
	h.eye_height = (cfg.height ? cfg.height : hi.stream_eye_height) & ~1u;
	for (float r: hi.available_refresh_rates)
		if (valid_refresh_rate(r))
			h.refresh_rates.push_back(r);
	h.preferred_refresh_rate = hi.settings.preferred_refresh_rate;
	if (not valid_refresh_rate(h.preferred_refresh_rate))
		h.preferred_refresh_rate = h.refresh_rates.empty() ? 72 : h.refresh_rates.front();
	if (h.eye_width < 16 or h.eye_height < 16)
		throw std::runtime_error("headset info: stream size " + std::to_string(h.eye_width) + "x" + std::to_string(h.eye_height));
	if (h.render_eye_width < 16 or h.render_eye_height < 16)
		h.render_eye_width = h.eye_width, h.render_eye_height = h.eye_height;
	for (int i = 0; i < 2; ++i)
		h.fov[i] = from_xr(hi.fov[i]);
	for (auto c: hi.supported_codecs)
	{
		switch (c)
		{
			case wivrn::h264:
				h.codecs.push_back(Codec::h264);
				break;
			case wivrn::h265:
				h.codecs.push_back(Codec::hevc);
				break;
			case wivrn::av1:
				h.codecs.push_back(Codec::av1);
				break;
			case wivrn::raw:
				h.codecs.push_back(Codec::raw);
				break;
		}
	}
	h.hand_tracking = hi.hand_tracking;
	h.eye_gaze = hi.eye_gaze;
	h.name = hi.system_name;
	if (hi.speaker and valid_audio(*hi.speaker))
		h.speaker = AudioFormat{hi.speaker->num_channels, hi.speaker->sample_rate};
	if (hi.microphone and valid_audio(*hi.microphone))
		h.microphone = AudioFormat{hi.microphone->num_channels, hi.microphone->sample_rate};
	refresh = h.preferred_refresh_rate;
	pacer.set_frame_duration(uint64_t(1e9 / h.preferred_refresh_rate));
	pacer.reset();
	wire_index.reset();

	// HEVC when the client lists it (all Quest/Pico/Frame clients do), else H.264.
	bool hevc = std::ranges::contains(h.codecs, Codec::hevc);
	bool h264 = std::ranges::contains(h.codecs, Codec::h264);
	if (not hevc and not h264)
		throw std::runtime_error("headset supports neither HEVC nor H.264");
	if (cfg.codec == WivrnConfig::Codec::h264 and h264)
		hevc = false; // asked for H.264 (a headset without it keeps HEVC: better than no stream)
	codec = hevc ? EncoderConfig::Codec::hevc : EncoderConfig::Codec::h264;
	{
		std::scoped_lock lock(encode_mutex, eye_mutex[0], eye_mutex[1]);
		for (int i = 0; i < 2; ++i)
		{
			encoders[i] = std::make_unique<VtEncoder>(EncoderConfig{
			        .codec = codec,
			        .width = h.eye_width,
			        .height = h.eye_height,
			        .fps = h.preferred_refresh_rate,
			        .bitrate_bps = cfg.bitrate_bps / 2,
			        .prioritize_speed = cfg.prioritize_speed,
			});
			idr[i].reset();
		}
	}
	{
		std::lock_guard lock(view_mutex);
		view_fov = h.fov;
	}

	auto wc = hevc ? wivrn::video_codec::h265 : wivrn::video_codec::h264;
	control->send(th::video_stream_description{
	        .width = uint16_t(h.eye_width),
	        .height = uint16_t(h.eye_height),
	        .codec = {wc, wc, wc},
	        .frame_rate = h.preferred_refresh_rate,
	        .refresh_rate = h.preferred_refresh_rate,
	});

	// Ask for head and controller poses predicted around the expected motion-to-photon time
	// (WiVRn's tracking_control::resolve pattern: 3 ms steps for head/hands).
	th::tracking_control tc{};
	const XrDuration latency = 40'000'000;
	for (auto dev: {wivrn::device_id::HEAD, wivrn::device_id::LEFT_GRIP, wivrn::device_id::LEFT_AIM,
	                wivrn::device_id::RIGHT_GRIP, wivrn::device_id::RIGHT_AIM})
	{
		for (XrDuration t = 0; t <= latency; t += (dev == wivrn::device_id::HEAD ? 3'000'000 : 10'000'000))
			tc.pattern.push_back({.device = dev, .prediction_ns = t});
	}
	tc.motions_to_photons = latency;
	control->send(std::move(tc));

	// Audio as WiVRn's audio_pipewire resume(): the streams the headset offered, in its own formats (the bridge
	// converts), and whether it should send its microphone.
	th::audio_stream_description ad{};
	if (h.speaker)
		ad.speaker = th::audio_stream_description::device{uint8_t(h.speaker->channels), h.speaker->rate};
	if (h.microphone)
		ad.microphone = th::audio_stream_description::device{uint8_t(h.microphone->channels), h.microphone->rate};
	if (ad.speaker or ad.microphone)
		control->send(std::move(ad));

	if (not cfg.applications.empty())
	{
		th::application_list apps{};
		for (auto & [id, name]: cfg.applications)
			apps.applications.push_back({id, name});
		control->send(std::move(apps));
	}

	{
		// with info, under the lock set_microphone takes: a switch while this runs is either read here or sent by
		// set_microphone, never lost in between
		std::lock_guard lock(cnx_mutex);
		info = h;
		battery.reset();
		if (ad.speaker or ad.microphone)
			control->send(th::feature_control{th::feature_control::microphone, h.microphone and mic_wanted.load()});
	}
	char buf[160];
	snprintf(buf, sizeof buf, "headset '%s': stream %ux%u per eye, render %ux%u (headset %ux%u), %.0f Hz, %s",
	         h.name.c_str(), h.eye_width, h.eye_height, h.render_eye_width, h.render_eye_height, h.nominal_render_width,
	         h.nominal_render_height, h.preferred_refresh_rate, hevc ? "HEVC" : "H.264");
	log(buf);
}

void WivrnSession::impl::handle(fh::packets && packet)
{
	std::visit(
	        [this](auto && pkt) {
		        using T = std::decay_t<decltype(pkt)>;
		        if constexpr (std::is_same_v<T, fh::timesync_response>)
		        {
			        clock.add(pkt.query, pkt.response);
			        stats.timesync++;
		        }
		        else if constexpr (std::is_same_v<T, fh::tracking>)
		        {
			        stats.tracking++;
			        ns t = clock.from_headset(pkt.timestamp);
			        std::vector<TrackedPose> poses;
			        for (auto & dp: pkt.device_poses)
			        {
				        bool ok;
				        Device d = to_device(dp.device, ok);
				        if (not ok)
					        continue;
				        TrackedPose tp{
				                .device = d,
				                .time = t,
				                .pose = from_xr(dp.pose),
				                .linear_velocity = {dp.linear_velocity.x, dp.linear_velocity.y, dp.linear_velocity.z},
				                .angular_velocity = {dp.angular_velocity.x, dp.angular_velocity.y, dp.angular_velocity.z},
				                .orientation_valid = bool(dp.flags & wivrn::from_headset::orientation_valid),
				                .position_valid = bool(dp.flags & wivrn::from_headset::position_valid),
				        };
				        if (tp.orientation_valid)
					        history[size_t(d)].add(tp);
				        poses.push_back(tp);
			        }
			        {
				        std::lock_guard lock(view_mutex);
				        for (int i = 0; i < 2; ++i)
				        {
					        view_pose[i] = from_xr(pkt.views[i].pose);
					        view_fov[i] = from_xr(pkt.views[i].fov);
				        }
			        }
			        if (listener and not poses.empty())
				        listener->on_tracking(poses);
		        }
		        else if constexpr (std::is_same_v<T, fh::feedback>)
		        {
			        stats.feedback++;
			        if (pkt.received_from_decoder and pkt.received_first_packet)
				        stats.last_feedback_decode_ns = pkt.received_from_decoder - pkt.received_first_packet;
			        if (auto id = wire_index.frame_id(pkt.frame_index))
			        {
				        auto paced = pkt;
				        paced.frame_index = *id;
				        pacer.on_feedback(paced, clock.offset());
			        }
			        if (pkt.stream_index < 2 and idr[pkt.stream_index].on_feedback(pkt.frame_index, pkt.sent_to_decoder != 0))
			        {
				        stats.idr_requests++;
				        log("IDR needed on stream " + std::to_string(pkt.stream_index) + " (frame " + std::to_string(pkt.frame_index) + " lost)");
			        }
		        }
		        else if constexpr (std::is_same_v<T, fh::inputs>)
		        {
			        if (listener)
			        {
				        std::vector<InputValue> v;
				        for (auto & i: pkt.values)
					        v.push_back({uint32_t(i.id), i.value, clock.from_headset(i.last_change_time)});
				        listener->on_input(v);
			        }
		        }
		        else if constexpr (std::is_same_v<T, fh::settings_changed>)
		        {
			        if (pkt.bitrate_bps)
			        {
				        std::scoped_lock lock(encode_mutex, eye_mutex[0], eye_mutex[1]);
				        for (auto & e: encoders)
					        if (e)
						        e->set_bitrate(pkt.bitrate_bps / 2);
			        }
		        }
		        else if constexpr (std::is_same_v<T, fh::refresh_rate_changed>)
		        {
			        if (not valid_refresh_rate(pkt.to))
				        return;
			        refresh = pkt.to;
			        pacer.set_frame_duration(uint64_t(1e9 / pkt.to));
			        if (listener)
				        listener->on_refresh_rate(pkt.to);
		        }
		        else if constexpr (std::is_same_v<T, wivrn::audio_data>)
		        {
			        stats.mic_packets++;
			        if (listener and pkt.payload.size() >= 2)
				        listener->on_microphone(
				                std::span(reinterpret_cast<const int16_t *>(pkt.payload.data()), pkt.payload.size() / 2),
				                clock.from_headset(pkt.timestamp));
		        }
		        else if constexpr (std::is_same_v<T, fh::battery>)
		        {
			        std::lock_guard lock(cnx_mutex);
			        if (pkt.present)
				        battery = Battery{pkt.charge, pkt.charging};
			        else
				        battery.reset();
		        }
		        else if constexpr (std::is_same_v<T, fh::get_application_list>)
		        {
			        th::application_list apps{.language = pkt.language, .country = pkt.country, .variant = pkt.variant};
			        for (auto & [id, name]: cfg.applications)
				        apps.applications.push_back({id, name});
			        control->send(std::move(apps));
		        }
		        else if constexpr (std::is_same_v<T, fh::start_app>)
		        {
			        bool known = std::ranges::any_of(cfg.applications, [&](auto & a) { return a.first == pkt.app_id; });
			        if (known)
			        {
				        std::lock_guard lock(cnx_mutex);
				        app_request = AppRequest{pkt.app_id, app_request.seq + 1};
			        }
			        log("headset asks to start app '" + pkt.app_id + "'" + (known ? "" : " (not offered, ignored)"));
		        }
		        else
		        {
			        // Remaining packets ( hand/body/face tracking, app launcher, battery, ...) are not used by M0.
		        }
	        },
	        std::move(packet));
}

void WivrnSession::impl::run_connection(wivrn::TCP && tcp)
{
	auto ctl = std::make_unique<control_socket>(std::move(tcp));
	auto strm = std::make_unique<stream_socket>();
	fh::headset_info_packet hi;
	handshake(*ctl, *strm, hi);
	if (not strm->get_fd() or cfg.tcp_only)
		strm.reset();
	{
		std::lock_guard lock(cnx_mutex);
		control = std::move(ctl);
		stream = std::move(strm);
	}
	clock.reset();
	for (auto & h: history)
		h.clear();
	on_headset_info(hi);
	is_connected = true;
	if (listener)
		listener->on_connected(*info);

	auto next_sync = std::chrono::steady_clock::now();
	while (not quit)
	{
		auto now = std::chrono::steady_clock::now();
		if (now >= next_sync)
		{
			next_sync = now + std::chrono::milliseconds(clock.interval());
			th::timesync_query q{.query = now_ns()};
			if (stream)
				stream->send(std::move(q));
			else
				control->send(std::move(q));
		}
		pollfd fds[2] = {{.fd = control->get_fd(), .events = POLLIN}, {.fd = stream ? stream->get_fd() : -1, .events = POLLIN}};
		int r = ::poll(fds, 2, 5);
		if (r < 0 and errno != EINTR)
			throw std::system_error(errno, std::system_category());
		if ((fds[0].revents & (POLLHUP | POLLERR)) and not(fds[0].revents & POLLIN))
			throw wivrn::socket_shutdown();
		if (fds[0].revents & POLLIN)
		{
			if (auto pkt = control->receive())
				handle(std::move(*pkt));
			while (auto pkt = control->receive_pending())
				handle(std::move(*pkt));
		}
		if (stream and (fds[1].revents & POLLIN))
		{
			if (auto pkt = stream->receive())
				handle(std::move(*pkt));
			while (auto pkt = stream->receive_pending())
				handle(std::move(*pkt));
		}
	}
}

void WivrnSession::impl::accept_loop()
{
	while (not quit)
	{
		pollfd pfd{.fd = tcp_listener->get_fd(), .events = POLLIN};
		if (::poll(&pfd, 1, 100) <= 0)
			continue;
		try
		{
			auto [tcp, addr] = tcp_listener->accept();
			char host[INET6_ADDRSTRLEN] = {};
			inet_ntop(AF_INET6, &addr.sin6_addr, host, sizeof host);
			log(std::string("connection from ") + host);
			run_connection(std::move(tcp));
		}
		catch (wivrn::socket_shutdown &)
		{
			log("headset disconnected");
		}
		catch (std::exception & e)
		{
			log(std::string("connection ended: ") + e.what());
		}
		bool was = is_connected.exchange(false);
		{
			std::lock_guard lock(cnx_mutex);
			control.reset();
			stream.reset();
			info.reset();
		}
		{
			std::scoped_lock lock(encode_mutex, eye_mutex[0], eye_mutex[1]);
			for (auto & e: encoders)
				e.reset();
		}
		if (was and listener)
			listener->on_disconnected();
	}
}

void WivrnSession::impl::send_frame_eye(int eye, const Frame & f, uint64_t wire, CVPixelBufferRef buf)
{
	VtEncoder * enc;
	bool idr_frame;
	EncodedFrame out;
	uint16_t width, height;
	{
		std::lock_guard lock(eye_mutex[eye]);
		enc = encoders[eye].get();
		if (not enc)
			return;
		width = uint16_t(enc->config().width);
		height = uint16_t(enc->config().height);
		idr_frame = idr[eye].frame_is_idr(wire);
		// periodic refresh (keyframe interval): VideoToolbox's low-latency rate control ignores
		// MaxKeyFrameIntervalDuration, so the IDR is forced here
		ns now = now_ns();
		if (cfg.keyframe_interval_s > 0 and now - last_idr_ns[eye] >= ns(cfg.keyframe_interval_s * 1e9))
			idr_frame = true;
		out = enc->encode(buf, f.display_time, idr_frame);
		if (out.idr)
			last_idr_ns[eye] = now;
	}
	if (out.annexb.empty())
		return; // dropped by the encoder
	stats.last_encode_ns = out.encode_end - out.encode_begin;

	th::video_stream_data_shard shard{};
	shard.stream_item_idx = uint8_t(eye);
	shard.frame_idx = wire;
	shard.shard_idx = 0;
	th::video_stream_data_shard::view_info_t vi{};
	vi.display_time = clock.to_headset(f.display_time);
	for (int i = 0; i < 2; ++i)
	{
		vi.pose[i] = to_xr(f.pose[i]);
		vi.fov[i] = to_xr(f.fov[i]);
		// The compositor's foveation (WiVRn foveation_parameter), or identity: one 1:1 band covering the whole axis
		// (stream_defoveator::defoveated_size = W x H).
		vi.foveation[i].x = f.foveation[i].x.empty() ? std::vector<uint16_t>{width} : f.foveation[i].x;
		vi.foveation[i].y = f.foveation[i].y.empty() ? std::vector<uint16_t>{height} : f.foveation[i].y;
	}
	vi.alpha = false;
	shard.view_info = vi;

	th::video_stream_data_shard::timing_info_t timing{
	        .encode_begin = clock.to_headset(out.encode_begin),
	        .encode_end = clock.to_headset(out.encode_end),
	        .send_begin = clock.to_headset(now_ns()),
	        .send_end = 0,
	};

	std::unique_lock lock(cnx_mutex);
	if (not control)
		return; // disconnected while encoding
	// All of a frame on one channel, as WiVRn's server does (its `control` SendData is only for the H.264 Vulkan
	// parameter sets): the client drops shards older than its current frame, so an IDR on TCP that arrives after
	// the next P frames on UDP was thrown away and the decoder fed P frames with no reference (live run with a
	// d3d11bench --vr frame stream: "Drop shard for old frame 1 (current 4)", then avcodec_send_packet failed
	// and the client closed the session). A lost IDR is re-requested by the feedback path (idr[]).
	bool on_control = not stream or cfg.tcp_only;
	ssize_t max_payload = on_control ? std::numeric_limits<uint32_t>::max() : th::video_stream_data_shard::max_payload_size;
	auto begin = out.annexb.begin(), end = out.annexb.end();
	while (begin != end)
	{
		size_t payload_size = std::max<ssize_t>(0, max_payload - ssize_t(wivrn::serialized_size(shard.view_info)));
		auto next = end - begin > ssize_t(payload_size) ? begin + payload_size : end;
		if (next == end)
		{
			timing.send_end = clock.to_headset(now_ns());
			shard.timing_info = timing;
		}
		shard.payload = {&*begin, size_t(next - begin)};
		try
		{
			if (on_control)
				control->send(th::video_stream_data_shard{shard});
			else
				stream->send(th::video_stream_data_shard{shard});
		}
		catch (...)
		{
			// network errors are handled by the receive loop
		}
		stats.bytes_sent += next - begin;
		++shard.shard_idx;
		shard.view_info.reset();
		begin = next;
	}
	stats.frames_sent++;
	if (out.idr)
		stats.idr_sent++;
}

// ---- public ----

WivrnSession::WivrnSession(WivrnConfig cfg) :
        p(std::make_unique<impl>())
{
	p->cfg = std::move(cfg);
	p->keys = std::make_unique<KnownKeys>(p->cfg.known_keys_file.empty() ? KnownKeys::default_dir() / "known_keys"
	                                                                     : std::filesystem::path(p->cfg.known_keys_file));
	if (p->cfg.encryption == WivrnConfig::Encryption::pairing)
		p->pin = p->cfg.pin.empty() ? KnownKeys::random_pin() : p->cfg.pin;
	std::random_device rd;
	char buf[17];
	snprintf(buf, sizeof buf, "%08x%08x", rd(), rd());
	p->cookie = buf;
}

WivrnSession::~WivrnSession()
{
	stop();
}

uint64_t WivrnSession::protocol_version()
{
	return wivrn::protocol_version;
}

void WivrnSession::start(SessionListener * l)
{
	p->listener = l;
	p->tcp_listener = std::make_unique<wivrn::TCPListener>(p->cfg.port);
	sockaddr_in6 a{};
	socklen_t len = sizeof a;
	getsockname(p->tcp_listener->get_fd(), (sockaddr *)&a, &len);
	p->bound_port = ntohs(a.sin6_port);
	if (p->cfg.advertise)
	{
		char proto[17];
		snprintf(proto, sizeof proto, "%016" PRIx64, wivrn::protocol_version);
		char host[256] = {};
		gethostname(host, sizeof host - 1);
		p->advertised_name = host;
		p->publisher = std::make_unique<DnssdPublisher>(host, "_wivrn._tcp", p->bound_port,
		                                                std::map<std::string, std::string>{
		                                                        {"protocol", proto},
		                                                        {"version", "steamac-vrhost (WiVRn v26.9 protocol)"},
		                                                        {"cookie", p->cookie},
		                                                });
	}
	p->quit = false;
	p->accept_thread = std::thread([this] { p->accept_loop(); });
}

void WivrnSession::stop()
{
	p->quit = true;
	if (p->accept_thread.joinable())
		p->accept_thread.join();
	p->publisher.reset();
	p->tcp_listener.reset();
}

bool WivrnSession::connected() const
{
	return p->is_connected;
}

std::optional<HeadsetInfo> WivrnSession::headset() const
{
	std::lock_guard lock(p->cnx_mutex);
	return p->info;
}

std::string WivrnSession::pin() const
{
	std::lock_guard lock(p->crypto_mutex);
	return p->pin;
}

WivrnConfig::Encryption WivrnSession::encryption() const
{
	std::lock_guard lock(p->crypto_mutex);
	return p->cfg.encryption;
}

// WiVRn's wivrn_server::set_encryption_state(pairing) / enable_pairing: a fresh random PIN each time pairing is
// turned on; off = paired headsets only. Encryption-disabled mode is left alone (no pairing in that mode).
bool WivrnSession::set_pairing(bool on)
{
	std::lock_guard lock(p->crypto_mutex);
	if (p->cfg.encryption == WivrnConfig::Encryption::disabled)
		return false;
	if (on and p->cfg.encryption != WivrnConfig::Encryption::pairing)
		p->pin = p->cfg.pin.empty() ? KnownKeys::random_pin() : p->cfg.pin;
	else if (not on)
		p->pin.clear();
	p->cfg.encryption = on ? WivrnConfig::Encryption::pairing : WivrnConfig::Encryption::enabled;
	return true;
}

std::string WivrnSession::advertised_name() const
{
	return p->advertised_name;
}

uint32_t WivrnSession::bitrate_bps() const
{
	std::lock_guard lock(p->encode_mutex);
	return p->cfg.bitrate_bps;
}

void WivrnSession::set_bitrate(uint32_t bps)
{
	std::scoped_lock lock(p->encode_mutex, p->eye_mutex[0], p->eye_mutex[1]);
	p->cfg.bitrate_bps = bps;
	for (auto & e: p->encoders)
		if (e)
			e->set_bitrate(bps / 2);
}

bool WivrnSession::request_refresh_rate(float hz)
{
	std::lock_guard lock(p->cnx_mutex);
	if (not p->info or not p->control or not std::ranges::any_of(p->info->refresh_rates, [&](float r) { return std::abs(r - hz) < 0.5f; }))
		return false;
	try
	{
		p->control->send(th::refresh_rate_change{.hz = hz});
	}
	catch (...)
	{
		return false; // the receive loop handles the disconnect
	}
	return true;
}

float WivrnSession::refresh_rate() const
{
	return p->is_connected ? p->refresh.load() : 0;
}

std::string WivrnSession::codec_name() const
{
	if (not p->is_connected)
		return "";
	std::lock_guard lock(p->encode_mutex);
	return p->codec == EncoderConfig::Codec::hevc ? "hevc" : "h264";
}

std::optional<WivrnSession::Battery> WivrnSession::battery() const
{
	std::lock_guard lock(p->cnx_mutex);
	return p->is_connected ? p->battery : std::nullopt;
}

std::optional<WivrnSession::PairingRequest> WivrnSession::pairing_request() const
{
	std::lock_guard lock(p->crypto_mutex);
	return p->pairing_request;
}

WivrnSession::AppRequest WivrnSession::app_request() const
{
	std::lock_guard lock(p->cnx_mutex);
	return p->app_request;
}

std::string WivrnSession::known_keys_file() const
{
	return p->keys->file();
}

int WivrnSession::port() const
{
	return p->bound_port;
}

const WivrnStats & WivrnSession::stats() const
{
	return p->stats;
}

FrameTiming WivrnSession::next_frame_timing()
{
	// WiVRn's pacer: the display time comes from the headset's feedback (decode and display times, phase locked to
	// the headset's blit), not from a fixed rate. Its wake-up time includes a 3 ms margin by which Monado expects the
	// application to wake early; steamac-vrhost is the application here, so it wakes that much before it.
	static constexpr ns app_margin = 3'000'000; // pacer.cpp margin_ns
	int64_t frame_id, wake_up, desired_present, slop, predicted_display;
	p->pacer.predict(frame_id, wake_up, desired_present, slop, predicted_display);
	ns wake = wake_up - app_margin;
	ns now = now_ns();
	if (wake > now)
	{
		timespec ts{.tv_sec = time_t((wake - now) / 1'000'000'000), .tv_nsec = long((wake - now) % 1'000'000'000)};
		nanosleep(&ts, nullptr);
	}
	return {.wake_up = wake, .predicted_display = predicted_display, .frame_id = uint64_t(frame_id)};
}

std::optional<TrackedPose> WivrnSession::pose_at(Device d, ns t)
{
	if (d >= Device::count)
		return std::nullopt;
	return p->history[size_t(d)].at(t);
}

bool WivrnSession::views_at(ns t, std::array<Pose, 2> & pose, std::array<Fov, 2> & fov)
{
	auto head = pose_at(Device::head, t);
	std::lock_guard lock(p->view_mutex);
	fov = p->view_fov;
	for (int i = 0; i < 2; ++i)
		pose[i] = head ? multiply(head->pose, p->view_pose[i]) : p->view_pose[i];
	return head.has_value();
}

void WivrnSession::submit(const Frame & f)
{
	if (not p->is_connected)
		return;
	// "present" for the pacer = the frame reaches the encoder (WiVRn marks it after the compositor's GPU submit).
	p->pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, int64_t(f.frame_id), now_ns());
	uint64_t wire = p->wire_index.assign(f.frame_id);
	std::array<CVPixelBufferRef, 2> buf{};
	for (int eye = 0; eye < 2; ++eye)
		if (f.eye[eye].surface and CVPixelBufferCreateWithIOSurface(nullptr, f.eye[eye].surface, nullptr, &buf[eye]) != kCVReturnSuccess)
			buf[eye] = nullptr;
	// Both eyes are encoded concurrently (two VideoToolbox sessions); the media engine serialises them partly, but
	// the second eye no longer waits for the first eye's send.
	std::exception_ptr err;
	std::thread right([&] {
		try
		{
			if (buf[1])
				p->send_frame_eye(1, f, wire, buf[1]);
		}
		catch (...)
		{
			err = std::current_exception();
		}
	});
	try
	{
		if (buf[0])
			p->send_frame_eye(0, f, wire, buf[0]);
	}
	catch (...)
	{
		if (not err)
			err = std::current_exception();
	}
	right.join();
	for (auto b: buf)
		if (b)
			CVPixelBufferRelease(b);
	if (err)
		std::rethrow_exception(err);
}

void WivrnSession::haptics(Device d, float duration_s, float frequency, float amplitude)
{
	// docs/vr-interfaces.md 5.3: NaN/negative -> 0, duration at most 5 s, amplitude 0..1; frequency 0 = the
	// runtime's default (OpenXR XR_FREQUENCY_UNSPECIFIED), duration 0 = the shortest pulse (XR_MIN_HAPTIC_DURATION).
	auto clean = [](float v, float hi) { return std::isfinite(v) ? std::clamp(v, 0.f, hi) : 0.f; };
	std::lock_guard lock(p->cnx_mutex);
	th::haptics h{
	        .id = from_device(d),
	        .duration = std::chrono::nanoseconds(int64_t(double(clean(duration_s, 5.f)) * 1e9)),
	        .frequency = clean(frequency, 1e5f),
	        .amplitude = clean(amplitude, 1.f),
	};
	p->stats.haptics++;
	try
	{
		if (p->stream)
			p->stream->send(std::move(h));
		else if (p->control)
			p->control->send(std::move(h));
	}
	catch (...)
	{
		// called from the link / audio threads: a headset that just left is the receive loop's to handle
	}
}

void WivrnSession::request_idr()
{
	for (auto & i: p->idr)
		i.request();
}
void WivrnSession::send_audio(std::span<const int16_t> pcm)
{
	if (pcm.empty() or not p->is_connected)
		return;
	std::lock_guard lock(p->cnx_mutex);
	if (not p->control)
		return;
	// Like WiVRn's speaker_process: one packet per quantum on the control channel, stamped with the send time.
	try
	{
		p->control->send(wivrn::audio_data{
		        .timestamp = p->clock.to_headset(now_ns()),
		        .payload = std::span(reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm.data())), pcm.size_bytes()),
		});
	}
	catch (...)
	{
		return; // the audio thread's send: a headset that just left is the receive loop's to handle
	}
	p->stats.audio_packets++;
}

void WivrnSession::set_microphone(bool on)
{
	if (p->mic_wanted.exchange(on) == on)
		return;
	std::lock_guard lock(p->cnx_mutex);
	if (p->control and p->info and p->info->microphone)
		try
		{
			p->control->send(th::feature_control{th::feature_control::microphone, on});
		}
		catch (...)
		{
			// as send_audio: the receive loop handles the disconnect
		}
}
} // namespace vrhost
