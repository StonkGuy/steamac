// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_discovery.h"

#include "vd/vd_wire.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vrhost::vd
{
std::vector<uint8_t> encode_offer(const Offer & o)
{
	BeWriter w;
	w.raw({reinterpret_cast<const uint8_t *>(o.rsa_public_xml.data()), o.rsa_public_xml.size()});
	if (o.platform)
	{
		w.u16(*o.platform);
		w.raw({reinterpret_cast<const uint8_t *>(o.account.data()), o.account.size()});
	}
	return w.out;
}

std::optional<Offer> parse_offer(std::span<const uint8_t> b)
{
	std::string s(b.begin(), b.end());
	if (not s.starts_with("<RSAKeyValue>"))
		return std::nullopt;
	auto end = s.find(rsa_key_end);
	if (end == std::string::npos)
		return std::nullopt;
	end += strlen(rsa_key_end);
	Offer o;
	o.rsa_public_xml = s.substr(0, end);
	if (s.size() >= end + 2)
	{
		o.platform = uint16_t(uint8_t(s[end]) << 8 | uint8_t(s[end + 1]));
		o.account = s.substr(end + 2);
	}
	return o;
}

std::vector<uint8_t> encode_connect_now(const ConnectNow & c)
{
	std::vector<uint8_t> out(c.guid.begin(), c.guid.end());
	out.push_back(c.platform);
	return out;
}

std::optional<ConnectNow> parse_connect_now(std::span<const uint8_t> b)
{
	if (b.size() != connect_now_size)
		return std::nullopt;
	ConnectNow c;
	std::copy_n(b.begin(), 16, c.guid.begin());
	c.platform = b[16];
	return c;
}

std::string guid_text(const std::array<uint8_t, 16> & g)
{
	char s[40];
	snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[0], g[1], g[2], g[3],
	         g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
	return s;
}

namespace
{
std::string escape(const std::string & v)
{
	std::string out;
	for (char c: v)
		switch (c)
		{
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			default: out += c;
		}
	return out;
}

std::string unescape(std::string v)
{
	for (auto [from, to]: {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}})
		for (size_t p; (p = v.find(from)) != std::string::npos;)
			v.replace(p, strlen(from), to);
	return v;
}

// All texts of <name>..</name> in order.
std::vector<std::string> elements(const std::string & xml, const std::string & name)
{
	std::vector<std::string> out;
	std::string open = "<" + name + ">", close = "</" + name + ">";
	for (size_t a = xml.find(open); a != std::string::npos; a = xml.find(open, a))
	{
		a += open.size();
		auto b = xml.find(close, a);
		if (b == std::string::npos)
			break;
		out.push_back(unescape(xml.substr(a, b - a)));
		a = b;
	}
	return out;
}

std::string element(const std::string & xml, const std::string & name)
{
	auto e = elements(xml, name);
	return e.empty() ? "" : e.front();
}
} // namespace

// TODO(vd-live:record-schema): our own element names, DataContract style.
std::string encode_record(const ComputerRecord & r)
{
	auto e = [](const char * n, const std::string & v) { return std::string("<") + n + ">" + escape(v) + "</" + n + ">"; };
	auto b = [](bool v) { return std::string(v ? "true" : "false"); };
	std::string x = "<Computer>";
	x += e("Id", r.id) + e("Name", r.name) + e("Description", r.description) + e("OS", r.os) + e("Version", r.version);
	x += "<PrivateAddresses>";
	for (auto & a: r.addresses)
		x += e("Address", a);
	x += "</PrivateAddresses>";
	x += e("ControlPort", std::to_string(r.control_port)) + e("DataPort", std::to_string(r.data_port)) +
	     e("VideoPort", std::to_string(r.video_port)) + e("AudioPort", std::to_string(r.audio_port)) +
	     e("UdpPort", std::to_string(r.discovery_port));
	x += e("AllowRemote", b(r.allow_remote)) + e("EncryptLocal", b(r.encrypt_local)) + e("EncryptRemote", b(r.encrypt_remote));
	return x + "</Computer>";
}

std::optional<ComputerRecord> parse_record(const std::string & x)
{
	if (not x.starts_with("<Computer>"))
		return std::nullopt;
	ComputerRecord r;
	r.id = element(x, "Id");
	r.name = element(x, "Name");
	r.description = element(x, "Description");
	r.os = element(x, "OS");
	r.version = element(x, "Version");
	r.addresses = elements(x, "Address");
	auto port = [&](const char * n) { return uint16_t(atoi(element(x, n).c_str())); };
	r.control_port = port("ControlPort");
	r.data_port = port("DataPort");
	r.video_port = port("VideoPort");
	r.audio_port = port("AudioPort");
	r.discovery_port = port("UdpPort");
	r.allow_remote = element(x, "AllowRemote") == "true";
	r.encrypt_local = element(x, "EncryptLocal") == "true";
	r.encrypt_remote = element(x, "EncryptRemote") == "true";
	return r;
}

std::optional<std::vector<uint8_t>> encode_reply(const AesKey & discovery, const AesKey & session,
                                                 const std::string & rsa_public_xml, const ComputerRecord & r)
{
	std::vector<uint8_t> k(session.key.begin(), session.key.end());
	k.insert(k.end(), session.iv.begin(), session.iv.end());
	auto out = rsa_encrypt(rsa_public_xml, k);
	if (not out)
		return std::nullopt;
	auto xml = encode_record(r);
	auto rec = aes_wrap(discovery, {reinterpret_cast<const uint8_t *>(xml.data()), xml.size()});
	out->insert(out->end(), rec.begin(), rec.end());
	return out;
}
} // namespace vrhost::vd
