// SPDX-License-Identifier: Apache-2.0
// Host link for the guest runtime: wmtr (production) or a Unix socket (vr-fakehost, tests).
#include "transport.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>
#include <string>

#include "log.h"

namespace svr {
namespace {
class SocketTransport : public Transport {
 public:
  explicit SocketTransport(int fd) : fd_(fd) {}
  ~SocketTransport() override { close(fd_); }
  int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) override {
    std::lock_guard<std::mutex> g(m_);
    if (send(fd_, req, req->size, MSG_NOSIGNAL) != (ssize_t)req->size) return -errno;
    ssize_t n = recv(fd_, rep, rep_cap, 0);
    if (n < (ssize_t)sizeof(svr_hdr)) return n < 0 ? -errno : -EPROTO;
    if (rep->size != (uint32_t)n) return -EPROTO;
    return 0;
  }
  int submit(const svr_wmtvr_submit &info) override {
    svr_submit_req req{};
    req.h = {SVR_SUBMIT, sizeof(req)};
    req.info = info;
    svr_ack ack{};
    int r = call(&req.h, &ack.h, sizeof(ack));
    return r ? r : ack.status;
  }
  const char *name() const override { return "socket"; }

 private:
  int fd_;
  std::mutex m_;
};

// int wmtr_vr_call(const void *req, uint32_t req_size, void *rep, uint32_t rep_cap): exported by the wmtr
// client (wmtr/src/wmtr_client.c, loaded by Wine as winemetal.so). Returns 0 or a negative errno.
typedef int (*wmtr_vr_call_fn)(const void *, uint32_t, void *, uint32_t);
// winemetal unix call entry (the wmtr client's __wine_unix_call_funcs[]): NTSTATUS (*)(void *params)
typedef uint32_t (*unix_call_fn)(void *);

class WmtrTransport : public Transport {
 public:
  WmtrTransport(wmtr_vr_call_fn fn, unix_call_fn submit) : fn_(fn), submit_(submit) {}
  int submit(const svr_wmtvr_submit &info) override {
    svr_wmtvr_submit params = info;  // struct unixcall_wmtvr_submit { struct WMTVRSubmitInfo info; }
    uint32_t st = submit_(&params);
    if (st) SVR_ERR("WMTVRSubmit: status %#x", st);
    return st ? -EIO : 0;
  }
  int call(const svr_hdr *req, svr_hdr *rep, uint32_t rep_cap) override {
    int r = fn_(req, req->size, rep, rep_cap);
    if (r) return r;
    if (rep->size < sizeof(svr_hdr) || rep->size > rep_cap) return -EPROTO;
    return 0;
  }
  const char *name() const override { return "wmtr"; }

 private:
  wmtr_vr_call_fn fn_;
  unix_call_fn submit_;
};

int find_winemetal(struct dl_phdr_info *info, size_t, void *out) {
  const char *n = info->dlpi_name;
  size_t len = n ? strlen(n) : 0;
  if (len >= 13 && !strcmp(n + len - 13, "/winemetal.so")) {
    *(std::string *)out = n;
    return 1;
  }
  return 0;
}

// Wine's own unixlib directory (where Proton keeps winemetal.so): the one holding the loaded ntdll.so
int find_unixlib_dir(struct dl_phdr_info *info, size_t, void *out) {
  const char *n = info->dlpi_name;
  size_t len = n ? strlen(n) : 0;
  if (len >= 9 && !strcmp(n + len - 9, "/ntdll.so")) {
    *(std::string *)out = std::string(n, len - 8);
    return 1;
  }
  return 0;
}
}  // namespace

std::unique_ptr<Transport> Transport::open_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return nullptr;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (connect(fd, (sockaddr *)&a, sizeof(a))) {
    SVR_ERR("cannot connect to %s: %s", path, strerror(errno));
    close(fd);
    return nullptr;
  }
  return std::unique_ptr<Transport>(new SocketTransport(fd));
}

std::unique_ptr<Transport> Transport::open_wmtr() {
  std::string path;
  dl_iterate_phdr(find_winemetal, &path);
  void *h = path.empty() ? nullptr : dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!h) {
    // DXMT not loaded in this process (Proton's steam.exe runs VR_Init for the registry before any game
    // renders): load Wine's winemetal.so ourselves for its own wmtr context. Only reached on VR init.
    path.clear();
    dl_iterate_phdr(find_unixlib_dir, &path);
    if (path.empty() || path.back() != '/') return nullptr;
    path += "winemetal.so";
    if (!(h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL))) {
      SVR_ERR("cannot load %s: %s", path.c_str(), dlerror());
      return nullptr;
    }
  }
  auto fn = (wmtr_vr_call_fn)dlsym(h, "wmtr_vr_call");
  auto table = (const void *const *)dlsym(h, "__wine_unix_call_funcs");
  if (!fn || !table) {
    SVR_ERR("%s has no wmtr_vr_call (wmtr client too old)", path.c_str());
    return nullptr;
  }
  return std::unique_ptr<Transport>(new WmtrTransport(fn, (unix_call_fn)table[SVR_WMTVR_SUBMIT_CALL]));
}

std::unique_ptr<Transport> Transport::open_default() {
  if (const char *s = getenv("STEAMAC_VR_SOCKET"); s && *s) return open_socket(s);
  return open_wmtr();
}
}  // namespace svr
