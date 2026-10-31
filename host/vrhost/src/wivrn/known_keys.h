// SPDX-License-Identifier: GPL-3.0-or-later
// Paired-headset store: WiVRn's known_keys (server/driver/configuration.cpp, known_keys.json) for steamac-vrhost.
// One headset per line, "<public key without PEM armour>\t<name>\t<last connection, unix seconds>". Default
// location: ~/Library/Application Support/steamac/vrhost/known_keys (0600); the running PIN is written next to it
// (file "pin", removed when pairing ends) so `steamac-vrhost --show-pin` can print it.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vrhost
{
struct HeadsetKey
{
	std::string public_key; // cleaned (no BEGIN/END lines, no whitespace), as WiVRn's clean_key()
	std::string name;
	int64_t last_connection = 0;
};

class KnownKeys
{
public:
	explicit KnownKeys(std::filesystem::path file);
	static std::filesystem::path default_dir();

	std::vector<HeadsetKey> list() const;
	bool contains(const std::string & clean_public_key) const;
	void add(HeadsetKey);
	void touch(const std::string & clean_public_key);
	// Removes headsets by name ("" = all); returns how many were removed.
	int remove(const std::string & name);
	const std::filesystem::path & file() const { return path; }

	static std::string clean_key(std::string key);
	static std::string random_pin(); // 6 digits, like WiVRn's set_encryption_state(pairing)

private:
	std::filesystem::path path;
	void save(const std::vector<HeadsetKey> &) const;
};
} // namespace vrhost
