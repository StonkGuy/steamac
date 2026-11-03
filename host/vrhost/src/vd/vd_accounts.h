// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: the (platform, account) pairs the user allowed to use this computer, the role of the
// Streamer's "Allow <platform> user '<account>'?" prompt (PROTOCOL.md section 2). One line per pair, tab separated:
// platform, last connection (unix seconds), account. File mode 0600; account names are never logged.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vrhost::vd
{
struct Account
{
	uint16_t platform = 0;
	std::string name;
	int64_t last_connection = 0;
};

class AccountStore
{
public:
	explicit AccountStore(std::filesystem::path file);
	std::vector<Account> list() const;
	bool allowed(uint16_t platform, const std::string & name) const;
	void allow(uint16_t platform, const std::string & name); // adds or refreshes last_connection
	int remove(const std::string & name);                    // "" = all
	const std::filesystem::path & file() const { return path; }

private:
	std::filesystem::path path;
	void save(const std::vector<Account> &) const;
};
} // namespace vrhost::vd
