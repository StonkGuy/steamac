// SPDX-License-Identifier: Apache-2.0
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace svr {
const Json *Json::get(const std::string &key) const {
  if (type != Object) return nullptr;
  for (auto &kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string Json::get_str(const std::string &key, const std::string &def) const {
  const Json *j = get(key);
  return j && j->type == String ? j->str : def;
}

namespace {
struct Parser {
  const char *p, *end;
  std::string err;

  void ws() {
    while (p < end) {
      if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
      } else if (*p == '/' && p + 1 < end && p[1] == '/') {  // tolerate comments (some binding files have them)
        while (p < end && *p != '\n') p++;
      } else {
        break;
      }
    }
  }
  bool fail(const char *what) {
    if (err.empty()) err = what;
    return false;
  }
  static void put_utf8(std::string &s, unsigned c) {
    if (c < 0x80) {
      s += (char)c;
    } else if (c < 0x800) {
      s += (char)(0xc0 | (c >> 6));
      s += (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      s += (char)(0xe0 | (c >> 12));
      s += (char)(0x80 | ((c >> 6) & 0x3f));
      s += (char)(0x80 | (c & 0x3f));
    } else {
      s += (char)(0xf0 | (c >> 18));
      s += (char)(0x80 | ((c >> 12) & 0x3f));
      s += (char)(0x80 | ((c >> 6) & 0x3f));
      s += (char)(0x80 | (c & 0x3f));
    }
  }
  bool hex4(unsigned *out) {
    if (end - p < 4) return fail("short \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return fail("bad \\u escape");
    }
    *out = v;
    return true;
  }
  bool string(std::string *s) {
    if (p >= end || *p != '"') return fail("expected string");
    p++;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c != '\\') {
        *s += c;
        continue;
      }
      if (p >= end) return fail("bad escape");
      char e = *p++;
      switch (e) {
        case '"': *s += '"'; break;
        case '\\': *s += '\\'; break;
        case '/': *s += '/'; break;
        case 'b': *s += '\b'; break;
        case 'f': *s += '\f'; break;
        case 'n': *s += '\n'; break;
        case 'r': *s += '\r'; break;
        case 't': *s += '\t'; break;
        case 'u': {
          unsigned c1;
          if (!hex4(&c1)) return false;
          if (c1 >= 0xd800 && c1 < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            unsigned c2;
            if (!hex4(&c2)) return false;
            c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
          }
          put_utf8(*s, c1);
          break;
        }
        default: return fail("bad escape");
      }
    }
    if (p >= end) return fail("unterminated string");
    p++;
    return true;
  }
  bool value(Json *v, int depth) {
    if (depth > 64) return fail("nesting too deep");
    ws();
    if (p >= end) return fail("unexpected end");
    char c = *p;
    if (c == '{') {
      p++;
      v->type = Json::Object;
      ws();
      if (p < end && *p == '}') return p++, true;
      for (;;) {
        ws();
        std::string key;
        if (!string(&key)) return false;
        ws();
        if (p >= end || *p != ':') return fail("expected ':'");
        p++;
        Json child;
        if (!value(&child, depth + 1)) return false;
        v->obj.emplace_back(std::move(key), std::move(child));
        ws();
        if (p < end && *p == ',') {
          p++;
          ws();
          if (p < end && *p == '}') return p++, true;  // trailing comma
          continue;
        }
        if (p < end && *p == '}') return p++, true;
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      p++;
      v->type = Json::Array;
      ws();
      if (p < end && *p == ']') return p++, true;
      for (;;) {
        Json child;
        if (!value(&child, depth + 1)) return false;
        v->arr.push_back(std::move(child));
        ws();
        if (p < end && *p == ',') {
          p++;
          ws();
          if (p < end && *p == ']') return p++, true;
          continue;
        }
        if (p < end && *p == ']') return p++, true;
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      v->type = Json::String;
      return string(&v->str);
    }
    if (end - p >= 4 && !strncmp(p, "true", 4)) return v->type = Json::Bool, v->b = true, p += 4, true;
    if (end - p >= 5 && !strncmp(p, "false", 5)) return v->type = Json::Bool, v->b = false, p += 5, true;
    if (end - p >= 4 && !strncmp(p, "null", 4)) return v->type = Json::Null, p += 4, true;
    if (c == '-' || (c >= '0' && c <= '9')) {
      std::string tmp;
      while (p < end && strchr("+-0123456789.eE", *p)) tmp += *p++;
      v->type = Json::Number;
      v->num = strtod(tmp.c_str(), nullptr);
      return true;
    }
    return fail("unexpected character");
  }
};
}  // namespace

bool Json::parse(const std::string &text, Json *out, std::string *err) {
  Parser ps{text.data(), text.data() + text.size(), {}};
  if (text.size() >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf)
    ps.p += 3;  // UTF-8 BOM
  *out = Json{};
  bool ok = ps.value(out, 0);
  if (ok) {
    ps.ws();
    if (ps.p != ps.end) ok = ps.fail("trailing data");
  }
  if (!ok && err) *err = ps.err + " at byte " + std::to_string(ps.p - text.data());
  return ok;
}

bool read_file(const std::string &path, std::string *out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  fclose(f);
  return true;
}
}  // namespace svr
