// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdarg>

namespace svr {
// Log to stderr (prefix "steamac-vr:") when STEAMAC_VR_LOG is set, or always for level 0 (errors).
// level 0 error, 1 info, 2 per-frame trace.
void logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// First call of a method the runtime does not implement: logged once per name at level 1.
void stub_once(const char *name);
}  // namespace svr

#define SVR_ERR(...) ::svr::logf(0, __VA_ARGS__)
#define SVR_INFO(...) ::svr::logf(1, __VA_ARGS__)
#define SVR_TRACE(...) ::svr::logf(2, __VA_ARGS__)
