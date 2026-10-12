// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual Desktop adapter: one of the three settings objects (user, mobile, streamer) the two sides mirror over the
// control channel (PROTOCOL.md section 5). Kept as top-level key -> raw JSON value text: the adapter reads a few
// scalars and passes everything else through untouched.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace vrhost::vd
{
class Settings
{
public:
	// Replaces the content with a JSON object's top-level members; false (content unchanged) if it is not one.
	bool parse(const std::string & json);
	std::string to_json() const; // keys sorted
	void set_raw(const std::string & key, const std::string & json_value) { values[key] = json_value; }
	void set(const std::string & key, bool v) { values[key] = v ? "true" : "false"; }
	void set(const std::string & key, double v);
	void set(const std::string & key, const std::string & v);
	std::optional<std::string> raw(const std::string & key) const;
	std::optional<double> number(const std::string & key) const;
	std::optional<bool> boolean(const std::string & key) const;
	std::optional<std::string> string(const std::string & key) const; // unescaped (\" \\ \/ only)
	bool empty() const { return values.empty(); }

	static std::string quote(const std::string &);

private:
	std::map<std::string, std::string> values;
};
} // namespace vrhost::vd
