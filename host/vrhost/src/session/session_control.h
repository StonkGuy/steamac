// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter-neutral pairing/status surface of a running session (A3, DESIGN.md section 2), for the control socket
// (docs/vr-interfaces.md section 5) and main.cpp. Each headset-protocol adapter provides one next to its VrSession,
// so neither the launcher nor ControlServer carries protocol knowledge.
#pragma once

#include "session/vr_session.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vrhost
{
class SessionControl
{
public:
	// waiting: only headsets/accounts paired before are accepted; pairing: a new one may pair now.
	enum class PairingState
	{
		waiting,
		pairing,
	};
	// Adapter-specific status entries, in order (e.g. WiVRn: port, bonjour, encryption, pin).
	struct Param
	{
		std::string key;
		std::variant<int64_t, std::string> value;
	};
	struct Peer
	{
		std::string name;
		int64_t last_connection = 0; // unix seconds
	};
	struct Counters
	{
		uint64_t frames_sent = 0, idr_sent = 0, bytes_sent = 0;
		int64_t last_encode_ns = 0, last_decode_ns = 0; // decode: headset-reported, 0 = unknown
	};

	virtual ~SessionControl() = default;
	virtual VrSession & session() = 0;
	virtual std::string protocol() const = 0; // "wivrn", "vd"
	virtual PairingState pairing_state() const = 0;
	virtual std::vector<Param> params() const = 0;
	// Pairing on/off at run time; false when the adapter cannot pair in its current mode.
	virtual bool set_pairing(bool on) = 0;
	// Secret the user enters on the headset while pairing ("" = none, e.g. the account is approved on the host).
	virtual std::string pin() const { return ""; }
	virtual std::vector<Peer> paired() const = 0;
	virtual int unpair(const std::string & name) = 0; // "" = all; returns how many were removed
	virtual uint32_t bitrate_bps() const = 0;          // configured total bitrate
	virtual Counters counters() const = 0;
};
} // namespace vrhost
