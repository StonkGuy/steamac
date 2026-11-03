// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_wire.h"

#include <algorithm>

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

std::vector<uint8_t> control_message(Msg type, std::span<const uint8_t> payload)
{
	BeWriter w;
	w.u8(uint8_t(type));
	w.u32(payload.size());
	w.raw(payload);
	return w.out;
}

std::vector<uint8_t> control_message(Msg type, const std::string & payload)
{
	return control_message(type, {reinterpret_cast<const uint8_t *>(payload.data()), payload.size()});
}

std::string setting_changed_payload(const std::string & name, const std::string & json_value)
{
	std::string q = "\"";
	for (char c: name)
	{
		if (c == '"' or c == '\\')
			q += '\\';
		q += c;
	}
	return "{" + q + "\":" + json_value + "}";
}

std::vector<uint8_t> video_format_message(const VideoFormat & f)
{
	std::vector<uint8_t> out{tag_video_format_old};
	auto b = bytes_of(f).first(video_format_old_size);
	out.insert(out.end(), b.begin(), b.end());
	return out;
}

std::vector<uint8_t> frame_message(const FrameMetadata & md, std::span<const uint8_t> data)
{
	std::vector<uint8_t> out{tag_frame};
	auto b = bytes_of(md);
	out.insert(out.end(), b.begin(), b.end());
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

std::vector<uint8_t> slice_message(const SliceHeader & sh, std::span<const uint8_t> data)
{
	std::vector<uint8_t> out{tag_slice};
	auto b = bytes_of(sh);
	out.insert(out.end(), b.begin(), b.end());
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

namespace
{
template <typename T>
T get(std::span<const uint8_t> b, size_t off)
{
	T v;
	memcpy(&v, b.data() + off, sizeof v);
	return v;
}
template <typename T>
void put(std::vector<uint8_t> & b, size_t off, const T & v)
{
	memcpy(b.data() + off, &v, sizeof v);
}
Quat get_quat(std::span<const uint8_t> b, size_t off)
{
	return {get<float>(b, off), get<float>(b, off + 4), get<float>(b, off + 8), get<float>(b, off + 12)};
}
Vec3 get_vec(std::span<const uint8_t> b, size_t off)
{
	return {get<float>(b, off), get<float>(b, off + 4), get<float>(b, off + 8)};
}
void put_quat(std::vector<uint8_t> & b, size_t off, const Quat & q)
{
	float v[4] = {q.x, q.y, q.z, q.w};
	memcpy(b.data() + off, v, sizeof v);
}
void put_vec(std::vector<uint8_t> & b, size_t off, const Vec3 & p)
{
	float v[3] = {p.x, p.y, p.z};
	memcpy(b.data() + off, v, sizeof v);
}
} // namespace

std::optional<TrackingSample> decode_tracking(std::span<const uint8_t> b)
{
	if (b.size() < frame_data_base)
		return std::nullopt;
	TrackingSample s;
	s.size = b.size();
	s.frame_index = get<int32_t>(b, fd_off::frame_index);
	s.latency_s = get<float>(b, fd_off::latency);
	s.floor_height = get<float>(b, fd_off::floor_height);
	size_t c = fd_off::controller;
	s.input.buttons = get<uint16_t>(b, c);
	s.input.touches = get<uint16_t>(b, c + 2);
	s.input.trigger[0] = b[c + 4];
	s.input.trigger[1] = b[c + 5];
	s.input.grip[0] = b[c + 6];
	s.input.grip[1] = b[c + 7];
	s.input.thumb_x[0] = get<int16_t>(b, c + 8);
	s.input.thumb_y[0] = get<int16_t>(b, c + 10);
	s.input.thumb_x[1] = get<int16_t>(b, c + 12);
	s.input.thumb_y[1] = get<int16_t>(b, c + 14);
	s.lock_states = b[fd_off::lock_states];
	size_t h = fd_off::head;
	s.head.orientation = get_quat(b, h);
	s.head.position = get_vec(b, h + 16);
	s.head.velocity = get_vec(b, h + 28);
	s.head.angular_velocity = get_vec(b, h + 40);
	s.head.tracked = b[h + 52] != 0;
	s.head.mounted = b[h + 53] != 0;
	s.head.battery = int8_t(b[h + 54]);
	for (int i = 0; i < 2; ++i)
	{
		size_t o = i ? fd_off::right_controller : fd_off::left_controller;
		auto & p = s.controller[i];
		p.orientation = get_quat(b, o);
		p.position = get_vec(b, o + 16);
		p.velocity = get_vec(b, o + 28);
		p.acceleration = get_vec(b, o + 40);
		p.angular_velocity = get_vec(b, o + 52);
		p.tracked = b[o + 64];
		p.battery = int8_t(b[o + 65]);
	}
	return s;
}

std::vector<uint8_t> encode_tracking(const TrackingSample & s, size_t size)
{
	std::vector<uint8_t> b(std::max(size, frame_data_base));
	put(b, fd_off::frame_index, s.frame_index);
	put(b, fd_off::latency, s.latency_s);
	put(b, fd_off::floor_height, s.floor_height);
	size_t c = fd_off::controller;
	put(b, c, s.input.buttons);
	put(b, c + 2, s.input.touches);
	b[c + 4] = s.input.trigger[0];
	b[c + 5] = s.input.trigger[1];
	b[c + 6] = s.input.grip[0];
	b[c + 7] = s.input.grip[1];
	put(b, c + 8, s.input.thumb_x[0]);
	put(b, c + 10, s.input.thumb_y[0]);
	put(b, c + 12, s.input.thumb_x[1]);
	put(b, c + 14, s.input.thumb_y[1]);
	b[fd_off::lock_states] = s.lock_states;
	size_t h = fd_off::head;
	put_quat(b, h, s.head.orientation);
	put_vec(b, h + 16, s.head.position);
	put_vec(b, h + 28, s.head.velocity);
	put_vec(b, h + 40, s.head.angular_velocity);
	b[h + 52] = s.head.tracked;
	b[h + 53] = s.head.mounted;
	b[h + 54] = uint8_t(s.head.battery);
	for (int i = 0; i < 2; ++i)
	{
		size_t o = i ? fd_off::right_controller : fd_off::left_controller;
		auto & p = s.controller[i];
		put_quat(b, o, p.orientation);
		put_vec(b, o + 16, p.position);
		put_vec(b, o + 28, p.velocity);
		put_vec(b, o + 40, p.acceleration);
		put_vec(b, o + 52, p.angular_velocity);
		b[o + 64] = p.tracked;
		b[o + 65] = uint8_t(p.battery);
	}
	return b;
}

std::vector<uint8_t> games_payload(std::span<const GameEntry> games)
{
	size_t n = std::min<size_t>(games.size(), 0x7fff);
	std::vector<uint8_t> out{uint8_t(n >> 8), uint8_t(n)};
	for (size_t i = 0; i < n; ++i)
	{
		out.push_back(uint8_t(games[i].platform));
		for (int b = 7; b >= 0; --b)
			out.push_back(uint8_t(games[i].id >> (8 * b)));
		out.push_back(games[i].running ? 1 : 0);
	}
	return out;
}

std::optional<GameEntry> parse_game_id(std::span<const uint8_t> b)
{
	if (b.size() < 9)
		return std::nullopt;
	GameEntry e{.platform = GamePlatform(b[0])};
	for (int i = 1; i < 9; ++i)
		e.id = e.id << 8 | b[i];
	return e;
}

std::vector<uint8_t> game_id_payload(GamePlatform p, uint64_t id)
{
	std::vector<uint8_t> out{uint8_t(p)};
	for (int b = 7; b >= 0; --b)
		out.push_back(uint8_t(id >> (8 * b)));
	return out;
}

std::optional<std::vector<GameEntry>> parse_games(std::span<const uint8_t> b)
{
	if (b.size() < 2)
		return std::nullopt;
	size_t n = size_t(b[0]) << 8 | b[1];
	if (b.size() != 2 + n * games_entry_size)
		return std::nullopt;
	std::vector<GameEntry> out;
	for (size_t i = 0; i < n; ++i)
	{
		auto e = parse_game_id(b.subspan(2 + i * games_entry_size));
		e->running = b[2 + i * games_entry_size + 9] != 0;
		out.push_back(*e);
	}
	return out;
}
} // namespace vrhost::vd
