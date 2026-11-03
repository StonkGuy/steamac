// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_accounts.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace vrhost::vd
{
AccountStore::AccountStore(std::filesystem::path p) : path(std::move(p)) {}

std::vector<Account> AccountStore::list() const
{
	std::vector<Account> out;
	std::ifstream f(path);
	for (std::string line; std::getline(f, line);)
	{
		std::istringstream s(line);
		Account a;
		std::string platform, last;
		if (std::getline(s, platform, '\t') and std::getline(s, last, '\t') and std::getline(s, a.name) and not a.name.empty())
		{
			a.platform = uint16_t(std::stoul(platform));
			a.last_connection = std::stoll(last);
			out.push_back(a);
		}
	}
	return out;
}

bool AccountStore::allowed(uint16_t platform, const std::string & name) const
{
	for (auto & a: list())
		if (a.platform == platform and a.name == name)
			return true;
	return false;
}

void AccountStore::allow(uint16_t platform, const std::string & name)
{
	auto all = list();
	std::erase_if(all, [&](const Account & a) { return a.platform == platform and a.name == name; });
	all.push_back({platform, name, int64_t(time(nullptr))});
	save(all);
}

int AccountStore::remove(const std::string & name)
{
	auto all = list();
	auto n = std::erase_if(all, [&](const Account & a) { return name.empty() or a.name == name; });
	save(all);
	return int(n);
}

void AccountStore::save(const std::vector<Account> & all) const
{
	std::filesystem::create_directories(path.parent_path());
	auto tmp = path;
	tmp += ".new";
	{
		std::ofstream f(tmp, std::ios::trunc);
		chmod(tmp.c_str(), 0600);
		for (auto & a: all)
			f << a.platform << '\t' << a.last_connection << '\t' << a.name << '\n';
	}
	std::filesystem::rename(tmp, path);
}
} // namespace vrhost::vd
