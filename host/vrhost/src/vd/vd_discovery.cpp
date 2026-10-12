// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_discovery.h"

#include "vd/vd_wire.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace vrhost::vd
{
std::vector<uint8_t> encode_offer(const Offer & o)
{
	std::vector<uint8_t> out(o.rsa_public_xml.begin(), o.rsa_public_xml.end());
	if (o.platform)
	{
		out.push_back(0);
		out.push_back(*o.platform);
	}
	out.insert(out.end(), o.account.begin(), o.account.end());
	return out;
}

std::optional<Offer> parse_offer(std::span<const uint8_t> b)
{
	if (b.size() < offer_key_chars)
		return std::nullopt;
	Offer o;
	o.rsa_public_xml.assign(b.begin(), b.begin() + offer_key_chars);
	if (not o.rsa_public_xml.starts_with("<RSAKeyValue>") or not o.rsa_public_xml.ends_with("</RSAKeyValue>"))
		return std::nullopt;
	auto rest = b.subspan(offer_key_chars);
	if (rest.size() >= 2 and rest[0] == 0)
	{
		o.platform = rest[1];
		rest = rest.subspan(2);
	}
	o.account.assign(rest.begin(), rest.end());
	return o;
}

std::vector<uint8_t> wrap_offer(const AesKey & discovery, const Offer & o)
{
	std::vector<uint8_t> plain[] = {encode_offer(o)};
	return chunk_wrap(discovery, plain);
}

std::optional<Offer> unwrap_offer(const AesKey & discovery, std::span<const uint8_t> d)
{
	if (d.size() <= offer_key_chars)
		return std::nullopt;
	auto plain = chunk_unwrap(discovery, d);
	if (not plain)
		return std::nullopt;
	return parse_offer(*plain);
}

std::vector<uint8_t> encode_connect_now(const ConnectNow & c)
{
	std::vector<uint8_t> out(c.connection_id.begin(), c.connection_id.end());
	out.push_back(c.platform);
	return out;
}

std::optional<ConnectNow> parse_connect_now(std::span<const uint8_t> b)
{
	if (b.size() != connect_now_size)
		return std::nullopt;
	ConnectNow c;
	std::copy_n(b.begin(), 16, c.connection_id.begin());
	c.platform = b[16];
	return c;
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

// Texts of every <name>..</name> (name may carry a prefix, e.g. "a:_Major"), in order.
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

const char * region_name(Region r)
{
	switch (r)
	{
		case Region::america_west: return "AmericaWest";
		case Region::america_central: return "AmericaCentral";
		case Region::europe: break;
	}
	return "Europe";
}
} // namespace

// .NET DataContract XML: members in ordinal order of their names, booleans as true/false, byte arrays base64,
// enums by name, System.Version by its private fields in the System namespace (VD-FACT(record-schema)).
std::string encode_record(const ComputerRecord & r)
{
	auto e = [](const char * n, const std::string & v) { return std::string("<") + n + ">" + escape(v) + "</" + n + ">"; };
	auto b = [](bool v) { return std::string(v ? "true" : "false"); };
	std::string x = std::string("<Computer xmlns=\"") + record_namespace +
	                "\" xmlns:i=\"http://www.w3.org/2001/XMLSchema-instance\">";
	x += e("AllowRemoteConnections", b(r.allow_remote));
	x += e("ConnectionID", base64(r.connection_id));
	x += e("Description", r.description);
	x += e("EncryptLocalTraffic", b(r.encrypt_local));
	x += e("EncryptRemoteTraffic", b(r.encrypt_remote));
	x += e("ID", r.id);
	x += e("IV", base64(r.channel_key.iv));
	x += e("Key", base64(r.channel_key.key));
	x += e("Name", r.name);
	x += e("OS", r.os);
	x += "<PrivateAdapters>";
	for (auto & a: r.adapters)
		x += "<NetworkAdapter>" + e("IPAddresString", a.ip) + e("IsGigabit", b(a.gigabit)) + e("IsUsb", b(a.usb)) +
		     e("IsWireless", b(a.wireless)) + e("MACAddresString", a.mac) + "</NetworkAdapter>";
	x += "</PrivateAdapters>";
	x += e("Region", region_name(r.region));
	x += "<StreamerVersion xmlns:a=\"http://schemas.datacontract.org/2004/07/System\">" +
	     e("a:_Build", std::to_string(r.version[2])) + e("a:_Major", std::to_string(r.version[0])) +
	     e("a:_Minor", std::to_string(r.version[1])) + e("a:_Revision", std::to_string(r.version[3])) + "</StreamerVersion>";
	return x + "</Computer>";
}

std::optional<ComputerRecord> parse_record(const std::string & x)
{
	if (not x.starts_with("<Computer ") or x.find(record_namespace) == std::string::npos)
		return std::nullopt;
	ComputerRecord r;
	r.id = element(x, "ID");
	r.name = element(x, "Name");
	r.description = element(x, "Description");
	r.os = element(x, "OS");
	r.version = {atoi(element(x, "a:_Major").c_str()), atoi(element(x, "a:_Minor").c_str()),
	             atoi(element(x, "a:_Build").c_str()), atoi(element(x, "a:_Revision").c_str())};
	for (auto & a: elements(x, "NetworkAdapter"))
		r.adapters.push_back({.ip = element(a, "IPAddresString"),
		                      .mac = element(a, "MACAddresString"),
		                      .wireless = element(a, "IsWireless") == "true",
		                      .gigabit = element(a, "IsGigabit") == "true",
		                      .usb = element(a, "IsUsb") == "true"});
	r.allow_remote = element(x, "AllowRemoteConnections") == "true";
	r.encrypt_local = element(x, "EncryptLocalTraffic") == "true";
	r.encrypt_remote = element(x, "EncryptRemoteTraffic") == "true";
	auto id = unbase64(element(x, "ConnectionID"));
	auto key = unbase64(element(x, "Key"));
	auto iv = unbase64(element(x, "IV"));
	if (not id or id->size() != 16 or not key or key->size() != 32 or not iv or iv->size() != 16)
		return std::nullopt;
	std::copy_n(id->begin(), 16, r.connection_id.begin());
	std::copy_n(key->begin(), 32, r.channel_key.key.begin());
	std::copy_n(iv->begin(), 16, r.channel_key.iv.begin());
	auto region = element(x, "Region");
	r.region = region == "AmericaWest" ? Region::america_west : region == "AmericaCentral" ? Region::america_central : Region::europe;
	return r;
}

std::optional<std::vector<uint8_t>> encode_reply(const std::string & rsa_public_xml, const ComputerRecord & r)
{
	AesKey wrapper = AesKey::random();
	std::vector<uint8_t> k(wrapper.key.begin(), wrapper.key.end());
	k.insert(k.end(), wrapper.iv.begin(), wrapper.iv.end());
	auto out = rsa_encrypt(rsa_public_xml, k);
	if (not out or out->size() != rsa_block_size)
		return std::nullopt;
	auto xml = encode_record(r);
	std::vector<uint8_t> payload[] = {{xml.begin(), xml.end()}};
	auto rec = chunk_wrap(wrapper, payload);
	out->insert(out->end(), rec.begin(), rec.end());
	return out;
}

std::optional<ComputerRecord> parse_reply(std::span<const uint8_t> d,
                                          const std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t>)> & rsa_decrypt)
{
	if (d.size() <= rsa_block_size)
		return std::nullopt;
	auto k = rsa_decrypt(d.first(rsa_block_size));
	if (not k or k->size() != 48)
		return std::nullopt;
	AesKey wrapper;
	std::copy_n(k->begin(), 32, wrapper.key.begin());
	std::copy_n(k->begin() + 32, 16, wrapper.iv.begin());
	auto xml = chunk_unwrap(wrapper, d.subspan(rsa_block_size));
	if (not xml)
		return std::nullopt;
	return parse_record(std::string(xml->begin(), xml->end()));
}
} // namespace vrhost::vd
