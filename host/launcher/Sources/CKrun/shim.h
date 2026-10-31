// libkrun v1.19.6 C API, found via -Xcc -I<libkrun include dir> (see build.sh).
#ifndef STEAMAC_CKRUN_SHIM_H
#define STEAMAC_CKRUN_SHIM_H

#include <libkrun.h>
#include <libkrun_display.h>
#include <libkrun_input.h>
#include <util.h>     // openpty
#include <termios.h>
#include <vraudio.h>  // VR audio region (host/vrhost/include, docs/vr-interfaces.md section 6)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Function-like/compound macros are not imported by Swift; re-export as constants.
static const uint32_t STEAMAC_COMPAT_NET_FEATURES = COMPAT_NET_FEATURES;
static const uint32_t STEAMAC_NET_FLAG_VFKIT = NET_FLAG_VFKIT;
static const uint32_t STEAMAC_VIRGL_VENUS = VIRGLRENDERER_VENUS;
static const uint32_t STEAMAC_VIRGL_NO_VIRGL = VIRGLRENDERER_NO_VIRGL;
static const uint32_t STEAMAC_KERNEL_FORMAT_RAW = KRUN_KERNEL_FORMAT_RAW;
static const uint32_t STEAMAC_DISK_FORMAT_RAW = KRUN_DISK_FORMAT_RAW;

static const size_t STEAMAC_VRA_REGION_BYTES = VRA_REGION_BYTES;

// A fresh VR audio region (shm_open is variadic, so not callable from Swift): an unlinked, initialised shared
// memory object of VRA_REGION_BYTES. Returns its fd (FD_CLOEXEC; spawn inherits it explicitly) or -errno.
static inline int steamac_vra_create(void) {
  char name[64];
  snprintf(name, sizeof name, "/steamac-vra.%d", (int)getpid());
  shm_unlink(name);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return -errno;
  shm_unlink(name);
  int err = 0;
  void *p = MAP_FAILED;
  if (ftruncate(fd, (off_t)VRA_REGION_BYTES) < 0 ||
      (p = mmap(NULL, VRA_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
    err = errno;
    close(fd);
    return -err;
  }
  vra_region_init((struct vra_region *)p);
  munmap(p, VRA_REGION_BYTES);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

#endif
