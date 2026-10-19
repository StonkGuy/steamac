// SPDX-License-Identifier: Apache-2.0
// Transport::open_wmtr in a process without DXMT (Proton's steam.exe doing VR_Init for the registry): the runtime
// must load winemetal.so from Wine's unixlib directory (beside the loaded ntdll.so) instead of failing with
// "no host link". Fake ntdll.so / winemetal.so are built by the Makefile into $(OUT)/transport/x86_64-unix.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "transport.h"

static int checks, failures;
#define CHECK(c)                                                 \
  do {                                                           \
    checks++;                                                    \
    if (!(c)) {                                                  \
      failures++;                                                \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
    }                                                            \
  } while (0)

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "build/transport/x86_64-unix";
  CHECK(!svr::Transport::open_wmtr());  // no Wine unixlib dir loaded: nothing to find
  void *nt = dlopen((dir + "/ntdll.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
  CHECK(nt);
  CHECK(!dlopen((dir + "/winemetal.so").c_str(), RTLD_NOW | RTLD_NOLOAD));  // DXMT not loaded yet
  auto t = svr::Transport::open_wmtr();
  CHECK(t && !strcmp(t->name(), "wmtr"));
  if (t) {
    svr_hello_req req{};
    req.h = {SVR_HELLO, sizeof(req)};
    svr_hello_rep rep{};
    CHECK(t->call(&req.h, &rep.h, sizeof(rep)) == 0 && rep.h.op == SVR_HELLO && rep.version == SVR_VERSION);
  }
  printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
