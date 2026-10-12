// SPDX-License-Identifier: GPL-3.0-or-later
#include "vd/vd_settings.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace vrhost::vd
{
namespace
{
void skip_ws(const std::string & s, size_t & i)
{
	while (i < s.size() and std::isspace((unsigned char)s[i]))
		++i;
}

// End of the JSON string starting at s[i] == '"' (index past the closing quote), npos if unterminated.
size_t string_end(const std::string & s, size_t i)
{
	for (++i; i < s.size(); ++i)
	{
		if (s[i] == '\\')
			++i;
		else if (s[i] == '"')
			return i + 1;
	}
	return std::string::npos;
}

// End of the JSON value starting at s[i]: strings, nested objects/arrays (bracket matching), scalars.
size_t value_end(const std::string & s, size_t i)
{
	if (i >= s.size())
		return std::string::npos;
	if (s[i] == '"')
		return string_end(s, i);
	if (s[i] == '{' or s[i] == '[')
	{
		int depth = 0;
		for (; i < s.size(); ++i)
		{
			if (s[i] == '"')
			{
				i = string_end(s, i);
				if (i == std::string::npos)
					return i;
				--i;
			}
			else if (s[i] == '{' or s[i] == '[')
				++depth;
			else if ((s[i] == '}' or s[i] == ']') and --depth == 0)
				return i + 1;
		}
		return std::string::npos;
	}
	size_t j = i;
	while (j < s.size() and s[j] != ',' and s[j] != '}' and not std::isspace((unsigned char)s[j]))
		++j;
	return j == i ? std::string::npos : j;
}

std::string unescape(const std::string & q) // q includes the quotes
{
	std::string out;
	for (size_t i = 1; i + 1 < q.size(); ++i)
	{
		if (q[i] == '\\' and i + 2 < q.size())
			++i;
		out += q[i];
	}
	return out;
}
} // namespace

bool Settings::parse(const std::string & s)
{
	std::map<std::string, std::string> out;
	size_t i = 0;
	skip_ws(s, i);
	if (i >= s.size() or s[i++] != '{')
		return false;
	skip_ws(s, i);
	if (i < s.size() and s[i] == '}')
	{
		values = {};
		return true;
	}
	while (i < s.size())
	{
		skip_ws(s, i);
		if (i >= s.size() or s[i] != '"')
			return false;
		size_t ke = string_end(s, i);
		if (ke == std::string::npos)
			return false;
		std::string key = unescape(s.substr(i, ke - i));
		i = ke;
		skip_ws(s, i);
		if (i >= s.size() or s[i++] != ':')
			return false;
		skip_ws(s, i);
		size_t ve = value_end(s, i);
		if (ve == std::string::npos)
			return false;
		out[key] = s.substr(i, ve - i);
		i = ve;
		skip_ws(s, i);
		if (i < s.size() and s[i] == ',')
		{
			++i;
			continue;
		}
		if (i < s.size() and s[i] == '}')
		{
			values = std::move(out);
			return true;
		}
		return false;
	}
	return false;
}

std::string Settings::to_json() const
{
	std::string out = "{";
	for (auto & [k, v]: values)
		out += (out.size() > 1 ? "," : "") + quote(k) + ":" + v;
	return out + "}";
}

std::string Settings::quote(const std::string & v)
{
	std::string out = "\"";
	for (char c: v)
	{
		if (c == '"' or c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

void Settings::set(const std::string & key, double v)
{
	char b[32];
	snprintf(b, sizeof b, "%.17g", v);
	values[key] = b;
}

void Settings::set(const std::string & key, const std::string & v)
{
	values[key] = quote(v);
}

std::optional<std::string> Settings::raw(const std::string & key) const
{
	auto it = values.find(key);
	if (it == values.end())
		return std::nullopt;
	return it->second;
}

std::optional<double> Settings::number(const std::string & key) const
{
	auto r = raw(key);
	if (not r or r->empty())
		return std::nullopt;
	char * end = nullptr;
	double v = strtod(r->c_str(), &end);
	if (end != r->c_str() + r->size())
		return std::nullopt;
	return v;
}

std::optional<bool> Settings::boolean(const std::string & key) const
{
	auto r = raw(key);
	if (r == "true")
		return true;
	if (r == "false")
		return false;
	return std::nullopt;
}

std::optional<std::string> Settings::string(const std::string & key) const
{
	auto r = raw(key);
	if (not r or r->size() < 2 or r->front() != '"')
		return std::nullopt;
	return unescape(*r);
}
} // namespace vrhost::vd
