// SPDX-License-Identifier: GPL-3.0-or-later
#include "wivrn/wivrn_control.h"

#include "wivrn/known_keys.h"

namespace vrhost
{
SessionControl::PairingState WivrnControl::pairing_state() const
{
	return s.encryption() == WivrnConfig::Encryption::pairing ? PairingState::pairing : PairingState::waiting;
}

std::vector<SessionControl::Param> WivrnControl::params() const
{
	using Enc = WivrnConfig::Encryption;
	Enc enc = s.encryption();
	return {
	        {"port", int64_t(s.port())},
	        {"bonjour", s.advertised_name()},
	        {"encryption", enc == Enc::disabled ? "disabled" : enc == Enc::pairing ? "pairing" : "enabled"},
	        {"pin", s.pin()},
	};
}

std::vector<SessionControl::Peer> WivrnControl::paired() const
{
	std::vector<Peer> out;
	for (auto & k: KnownKeys(s.known_keys_file()).list())
		out.push_back({k.name, int64_t(k.last_connection)});
	return out;
}

int WivrnControl::unpair(const std::string & name)
{
	return KnownKeys(s.known_keys_file()).remove(name);
}

SessionControl::Counters WivrnControl::counters() const
{
	auto & st = s.stats();
	return {
	        .frames_sent = st.frames_sent.load(),
	        .idr_sent = st.idr_sent.load(),
	        .idr_requests = st.idr_requests.load(),
	        .bytes_sent = st.bytes_sent.load(),
	        .last_encode_ns = st.last_encode_ns.load(),
	        .last_decode_ns = st.last_feedback_decode_ns.load(),
	};
}

std::optional<SessionControl::Battery> WivrnControl::battery() const
{
	if (auto b = s.battery())
		return Battery{b->charge, b->charging};
	return std::nullopt;
}

std::optional<SessionControl::PairingRequest> WivrnControl::pairing_request() const
{
	if (auto r = s.pairing_request())
		return PairingRequest{r->name, r->at, r->refused};
	return std::nullopt;
}

SessionControl::AppRequest WivrnControl::app_request() const
{
	auto a = s.app_request();
	return {a.id, a.seq};
}
} // namespace vrhost
