// SPDX-License-Identifier: GPL-3.0-or-later
// clean_key() and the PIN format follow WiVRn v26.9 server/driver/wivrn_connection.cpp and server/main.cpp.
#include "wivrn/known_keys.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>

namespace vrhost
{
KnownKeys::KnownKeys(std::filesystem::path file) :
        path(std::move(file))
{
}

std::filesystem::path KnownKeys::default_dir()
{
	const char * home = std::getenv("HOME");
	return std::filesystem::path(home ? home : "/tmp") / "Library/Application Support/steamac/vrhost";
}

std::string KnownKeys::clean_key(std::string key)
{
	static const std::regex header{"^-+BEGIN .*-+$", std::regex_constants::multiline};
	static const std::regex footer{"^-+END .*-+$", std::regex_constants::multiline};
	static const std::regex whitespace{"[[:space:]]"};
	key = std::regex_replace(key, header, "");
	key = std::regex_replace(key, footer, "");
	key = std::regex_replace(key, whitespace, "");
	return key;
}

std::string KnownKeys::random_pin()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> distrib(0, 999999);
	char buffer[7];
	snprintf(buffer, 7, "%06d", distrib(gen));
	return buffer;
}

std::vector<HeadsetKey> KnownKeys::list() const
{
	std::vector<HeadsetKey> out;
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line))
	{
		std::istringstream ss(line);
		HeadsetKey k;
		std::string ts;
		if (not std::getline(ss, k.public_key, '\t') or k.public_key.empty())
			continue;
		std::getline(ss, k.name, '\t');
		if (std::getline(ss, ts, '\t'))
			k.last_connection = std::atoll(ts.c_str());
		out.push_back(std::move(k));
	}
	return out;
}

bool KnownKeys::contains(const std::string & key) const
{
	for (auto & k: list())
		if (k.public_key == key)
			return true;
	return false;
}

void KnownKeys::save(const std::vector<HeadsetKey> & keys) const
{
	std::filesystem::create_directories(path.parent_path());
	auto tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::trunc);
		for (auto & k: keys)
		{
			std::string name = k.name;
			std::erase_if(name, [](char c) { return c == '\t' or c == '\n'; });
			f << k.public_key << '\t' << name << '\t' << k.last_connection << '\n';
		}
		if (not f)
			throw std::runtime_error("cannot write " + tmp.string());
	}
	chmod(tmp.c_str(), 0600);
	std::filesystem::rename(tmp, path);
}

static int64_t unix_now()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void KnownKeys::add(HeadsetKey key)
{
	auto keys = list();
	std::erase_if(keys, [&](auto & k) { return k.public_key == key.public_key; });
	key.last_connection = unix_now();
	keys.push_back(std::move(key));
	save(keys);
}

void KnownKeys::touch(const std::string & key)
{
	auto keys = list();
	for (auto & k: keys)
		if (k.public_key == key)
			k.last_connection = unix_now();
	save(keys);
}

int KnownKeys::remove(const std::string & name)
{
	auto keys = list();
	int n = std::erase_if(keys, [&](auto & k) { return name.empty() or k.name == name; });
	save(keys);
	return n;
}
} // namespace vrhost
