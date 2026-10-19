// SPDX-License-Identifier: GPL-3.0-or-later
// Bonjour (dns_sd) replacement for WiVRn's avahi_publisher: _wivrn._tcp with TXT protocol/version/cookie.
#pragma once
#include <map>
#include <memory>
#include <string>

namespace vrhost
{
class DnssdPublisher
{
public:
	DnssdPublisher(const std::string & name, const std::string & type, int port, const std::map<std::string, std::string> & txt);
	~DnssdPublisher();
	bool registered() const; // daemon confirmed the registration

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace vrhost
