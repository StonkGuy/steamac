// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: reads the LAN discovery key (AES-256 key + IV, a constant of the official apps) from the
// user's own installed Streamer at start-up (VD-FACT(key-source)). The value is never written anywhere by this code:
// it is returned in memory only and callers must not log it.
//
// How: the Streamer's network assembly is a .NET PE file. Its discovery class initialises two static byte arrays
// (32 and 16 bytes) in its static constructor from field-RVA data. We parse just enough ECMA-335 metadata to find
// that class, its .cctor body, the two `ldc.i4.s N; newarr; dup; ldtoken <field>` sequences, and the field-RVA rows,
// then copy the bytes. Nothing of the vendor's is executed.
#pragma once

#include "vd/vd_crypto.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vrhost::vd
{
struct StreamerKeyResult
{
	std::optional<AesKey> key;
	std::string error; // why not (never contains key material)
};

// From one assembly file; `type_name` = the discovery class (namespace-less name).
StreamerKeyResult read_streamer_key(const std::filesystem::path & assembly, const std::string & type_name = "ComputerDiscoveryClient");
// The installed Streamer's network assembly, in the order tried: /Applications, ~/Applications; arm64 then x86_64.
std::vector<std::filesystem::path> streamer_assembly_candidates();
// First candidate that yields a key.
StreamerKeyResult read_installed_streamer_key();
} // namespace vrhost::vd
