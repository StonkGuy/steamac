// SPDX-License-Identifier: GPL-3.0-or-later
// SessionControl of the WiVRn adapter: PIN pairing, the known_keys store, Bonjour name, port.
#pragma once

#include "session/session_control.h"
#include "wivrn/wivrn_session.h"

namespace vrhost
{
class WivrnControl final : public SessionControl
{
public:
	explicit WivrnControl(WivrnSession & s) : s(s) {}
	VrSession & session() override { return s; }
	std::string protocol() const override { return "wivrn"; }
	PairingState pairing_state() const override;
	std::vector<Param> params() const override;
	bool set_pairing(bool on) override { return s.set_pairing(on); }
	std::string pin() const override { return s.pin(); }
	std::vector<Peer> paired() const override;
	int unpair(const std::string & name) override;
	uint32_t bitrate_bps() const override { return s.bitrate_bps(); }
	Counters counters() const override;

private:
	WivrnSession & s;
};
} // namespace vrhost
