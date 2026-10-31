// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>

#include "steamac_vr_guest.h"

namespace svr {
class Transport {
 public:
  virtual ~Transport() = default;
  // one request, one reply; returns 0 or a negative errno. Thread-safe.
  virtual int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) = 0;
  // hand one frame to the host compositor (wmtr: WMTVRSubmit, asynchronous; socket: SVR_SUBMIT). 0 or -errno.
  virtual int submit(const svr_wmtvr_submit &info) = 0;
  virtual const char *name() const = 0;

  // STEAMAC_VR_SOCKET=<path> selects the socket transport, otherwise wmtr; nullptr if neither is available.
  static std::unique_ptr<Transport> open_default();
  static std::unique_ptr<Transport> open_socket(const char *path);
  static std::unique_ptr<Transport> open_wmtr();
};
}  // namespace svr
