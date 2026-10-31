// SPDX-License-Identifier: Apache-2.0
// Minimal JSON reader for action manifests and binding files (UTF-8 in, no writer).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace svr {
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;  // keeps file order

  const Json *get(const std::string &key) const;  // object member or nullptr
  std::string get_str(const std::string &key, const std::string &def = "") const;
  bool is_obj() const { return type == Object; }
  bool is_arr() const { return type == Array; }

  // returns false and sets *err on a syntax error
  static bool parse(const std::string &text, Json *out, std::string *err);
};

bool read_file(const std::string &path, std::string *out);
}  // namespace svr
