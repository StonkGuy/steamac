# Vendored WiVRn code

* Source: https://github.com/WiVRn/WiVRn, tag **v26.9**, commit `bbc6e4cc36c355fa6180980abd231673dc15115d`.
* Licence: GPL-3.0-or-later (`host/vrhost/COPYING`); `external/magic_enum.hpp` is MIT (v0.9.5, as vendored by WiVRn).
* Files: `common/{wivrn_packets.h, wivrn_serialization.h, wivrn_serialization_types.h, packed_quaternion.h,
  protocol_version.h, crypto.*, smp.*, secrets.*, wivrn_sockets.*}`, `server/compositor/pacer.{h,cpp}`,
  `server/driver/clock_offset.h` and `external/magic_enum.hpp`, **unmodified** except one hardening change marked
  `steamac` in `common/wivrn_serialization.h`: a vector of non-trivial elements reserves at most the bytes left in the
  packet instead of the element count from the wire (up to ~2^31; found by `tests/fuzz/fuzz_packets`). Re-apply it on
  a re-vendor unless WiVRn has an equivalent check.
* The pacer builds against minimal Monado stand-ins in `host/vrhost/compat/monado/` (`main/comp_target.h` timing-point
  enum, `util/u_var.h` no-op debug-GUI registration, `util/u_debug.h` env option, `os/os_time.h` = the host clock
  `CLOCK_UPTIME_RAW`) and `src/wivrn/clock_offset_compat.cpp` (the two conversion functions of
  `server/driver/clock_offset.cpp`). `std::jthread` needs `-fexperimental-library` with Apple's libc++.
* Build-time dependencies fetched by CMake at the versions WiVRn v26.9 uses: OpenXR-SDK `release-1.1.58` headers
  (the protocol hash depends on their array sizes) and Boost.PFR `boost-1.89.0`. OpenSSL: libcrypto 3.5.4 built statically from the
  pinned release tarball (SHA-256 in `CMakeLists.txt`) into the build directory.
* macOS shims live outside the vendored tree, in `host/vrhost/compat/wivrn_macos_compat.h` (force-included):
  `mmsghdr`/`recvmmsg`/`sendmmsg`, `MSG_NOSIGNAL` = 0 (the process ignores `SIGPIPE`), `IPV6_ADD/DROP_MEMBERSHIP`.
  `-Wno-c++11-narrowing` because `msghdr::msg_iovlen` is an `int` on macOS.
* Known macOS difference not shimmed: `UDP::receive_from_raw()` sizes its buffer with
  `recvfrom(MSG_PEEK | MSG_TRUNC)`, which returns the datagram length on Linux and 0 on macOS. steamac-vrhost does not
  call it (the handshake peeks the sender with `recvfrom(MSG_PEEK)` and lets `UDP::receive()` read and decrypt).
* macOS reports the IPv4 sender of a datagram on the dual-stack (AF_INET6) stream socket as a `sockaddr_in`, where Linux
  gives a v4-mapped `sockaddr_in6`; the handshake maps it before comparing it with the control socket's peer
  (`stream_sender()` in `src/wivrn/wivrn_session.cpp`, loopback test case 8).
* Protocol hash of this tree: `fc26e619b7ab80f8` (`steamac-vrhost --protocol`, pinned in `tests/test_protocol.cpp`).
  WiVRn `master` at `bc3e374` (2026-10-11) has no packet/serialization change since v26.9 (comment-only diff).

## Re-vendoring

Copy the same files from the new tag, rebuild, run `ctest`. `test_protocol` fails on purpose when the hash changes:
update the constant there and in `docs/vr.md` together with the client version users must install.
