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
	bool set_bitrate(uint32_t bps) override
	{
		s.set_bitrate(bps);
		return true;
	}
	bool request_refresh_rate(float hz) override { return s.request_refresh_rate(hz); }
	float refresh_rate() const override { return s.refresh_rate(); }
	std::string codec_name() const override { return s.codec_name(); }
	std::optional<Battery> battery() const override;
	std::optional<PairingRequest> pairing_request() const override;
	AppRequest app_request() const override;

private:
	WivrnSession & s;
};
} // namespace vrhost
