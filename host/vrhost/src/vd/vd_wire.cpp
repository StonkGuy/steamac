// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_wire.h"

namespace vrhost::vd
{
void BeWriter::u16(uint16_t v)
{
	u8(v >> 8);
	u8(v);
}
void BeWriter::u32(uint32_t v)
{
	u16(v >> 16);
	u16(v);
}
void BeWriter::u64(uint64_t v)
{
	u32(v >> 32);
	u32(v);
}
void BeWriter::str(const std::string & s)
{
	if (s.size() > 0xffff)
		throw std::runtime_error("vd: string too long");
	u16(s.size());
	out.insert(out.end(), s.begin(), s.end());
}
void BeWriter::address(std::span<const uint8_t> addr, uint32_t port)
{
	if (addr.size() != 4 and addr.size() != 16)
		throw std::runtime_error("vd: bad address size");
	u8(addr.size());
	raw(addr);
	u32(port);
}

std::span<const uint8_t> BeReader::raw(size_t n)
{
	if (left() < n)
		throw std::runtime_error("vd: short message");
	auto r = b.subspan(pos, n);
	pos += n;
	return r;
}
uint8_t BeReader::u8()
{
	return raw(1)[0];
}
uint16_t BeReader::u16()
{
	auto r = raw(2);
	return uint16_t(r[0] << 8 | r[1]);
}
uint32_t BeReader::u32()
{
	uint32_t hi = u16();
	return hi << 16 | u16();
}
uint64_t BeReader::u64()
{
	uint64_t hi = u32();
	return hi << 32 | u32();
}
std::string BeReader::str()
{
	auto r = raw(u16());
	return {r.begin(), r.end()};
}
std::vector<uint8_t> BeReader::address(uint32_t & port)
{
	uint8_t family = u8();
	if (family != 4 and family != 16)
		throw std::runtime_error("vd: bad address family");
	auto r = raw(family);
	port = u32();
	return {r.begin(), r.end()};
}

void put_varint(std::vector<uint8_t> & out, uint32_t v)
{
	while (v >= 0x80)
	{
		out.push_back(uint8_t(v) | 0x80);
		v >>= 7;
	}
	out.push_back(uint8_t(v));
}

std::optional<uint32_t> get_varint(std::span<const uint8_t> b, size_t & used)
{
	uint32_t v = 0;
	for (size_t i = 0; i < b.size() and i < 5; ++i)
	{
		v |= uint32_t(b[i] & 0x7f) << (7 * i);
		if (not(b[i] & 0x80))
		{
			used = i + 1;
			return v;
		}
	}
	return std::nullopt;
}

std::vector<uint8_t> encode_peer_info(const PeerInfo & p)
{
	BeWriter w;
	w.address(p.address, p.port);
	w.u8(p.encrypt);
	return w.out;
}

std::vector<uint8_t> control_message(Msg type, std::span<const uint8_t> payload)
{
	BeWriter w;
	w.u8(uint8_t(type));
	w.u32(payload.size());
	w.raw(payload);
	return w.out;
}

std::vector<uint8_t> setting_changed_payload(const std::string & name, const std::string & json_value)
{
	BeWriter w;
	w.str(name);
	w.str(json_value);
	return w.out;
}
} // namespace vrhost::vd
