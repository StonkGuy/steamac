// SPDX-License-Identifier: GPL-3.0-or-later
// Length-prefixed (AVCC/HVCC, as VideoToolbox outputs) to Annex-B, as WiVRn clients expect: start codes, and the
// parameter sets (VPS/SPS/PPS for HEVC) in-band before every IDR.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace vrhost::annexb
{
// Appends 00 00 00 01 + nal.
void append_nal(std::vector<uint8_t> & out, std::span<const uint8_t> nal);

// Converts a buffer of NAL units each prefixed by a big-endian length of `length_size` bytes (1, 2 or 4).
// Returns false on a malformed buffer (out is then partial).
bool append_length_prefixed(std::vector<uint8_t> & out, std::span<const uint8_t> data, int length_size);

// Splits an Annex-B stream into NAL payloads (without start codes). Accepts 3- and 4-byte start codes.
std::vector<std::span<const uint8_t>> split(std::span<const uint8_t> stream);

// HEVC nal_unit_type (bits 1..6 of the first header byte).
inline int hevc_nal_type(std::span<const uint8_t> nal)
{
	return nal.empty() ? -1 : (nal[0] >> 1) & 0x3f;
}
enum hevc_type : int
{
	hevc_idr_w_radl = 19,
	hevc_idr_n_lp = 20,
	hevc_cra = 21,
	hevc_vps = 32,
	hevc_sps = 33,
	hevc_pps = 34,
};
inline bool hevc_is_irap(int type)
{
	return type >= 16 and type <= 23;
}
} // namespace vrhost::annexb
