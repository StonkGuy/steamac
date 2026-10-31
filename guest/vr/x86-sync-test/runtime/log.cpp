// SPDX-License-Identifier: Apache-2.0
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace svr {
static int log_level() {
  static int level = [] {
    const char *s = getenv("STEAMAC_VR_LOG");
    return s && *s ? atoi(s) : 0;
  }();
  return level;
}

void logf(int level, const char *fmt, ...) {
  if (level > log_level()) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "steamac-vr: %s\n", buf);
}

void stub_once(const char *name) {
  static std::mutex m;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> g(m);
  if (seen.insert(name).second) logf(1, "unimplemented %s", name);
}
}  // namespace svr
