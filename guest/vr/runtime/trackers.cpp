// SPDX-License-Identifier: Apache-2.0
#include "trackers.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace svr {
namespace {
struct RoleName {
  const char *name, *setting;
};
const RoleName kRoles[SVR_TRK_ROLE_COUNT] = {
    {"", "TrackerRole_None"},
    {"handed", "TrackerRole_Handed"},
    {"left_foot", "TrackerRole_LeftFoot"},
    {"right_foot", "TrackerRole_RightFoot"},
    {"left_shoulder", "TrackerRole_LeftShoulder"},
    {"right_shoulder", "TrackerRole_RightShoulder"},
    {"left_elbow", "TrackerRole_LeftElbow"},
    {"right_elbow", "TrackerRole_RightElbow"},
    {"left_knee", "TrackerRole_LeftKnee"},
    {"right_knee", "TrackerRole_RightKnee"},
    {"waist", "TrackerRole_Waist"},
    {"chest", "TrackerRole_Chest"},
    {"camera", "TrackerRole_Camera"},
    {"keyboard", "TrackerRole_Keyboard"},
    {"left_wrist", "TrackerRole_LeftWrist"},
    {"right_wrist", "TrackerRole_RightWrist"},
    {"left_ankle", "TrackerRole_LeftAnkle"},
    {"right_ankle", "TrackerRole_RightAnkle"},
};

bool iequal(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

// "LeftFoot" -> "left_foot"
std::string snake(const char *camel) {
  std::string s;
  for (const char *p = camel; *p; p++) {
    if (isupper((unsigned char)*p) && p != camel) s += '_';
    s += (char)tolower((unsigned char)*p);
  }
  return s;
}

uint32_t load_acq(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

void seq_begin(uint32_t *seq) {
  __atomic_store_n(seq, __atomic_load_n(seq, __ATOMIC_RELAXED) | 1u, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
}
void seq_end(uint32_t *seq) {
  uint32_t s = __atomic_load_n(seq, __ATOMIC_RELAXED);
  __atomic_store_n(seq, (s | 1u) + 1u, __ATOMIC_RELEASE);  // even again, and different from before the write
}

// copies `size` bytes from a seqlocked object; returns the (even) sequence or 0 after too many retries
template <typename T>
uint32_t seq_read(const T *src, const uint32_t *seq, T *out) {
  for (int tries = 0; tries < 1000; tries++) {
    uint32_t s1 = load_acq(seq);
    if (s1 & 1u) continue;
    memcpy((void *)out, (const void *)src, sizeof(T));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(seq, __ATOMIC_RELAXED) == s1) return s1 ? s1 : 2;  // 0 only means "never written"
  }
  return 0;
}

void copy_str(char *dst, size_t cap, const char *src) {
  size_t n = strnlen(src, cap - 1);
  memcpy(dst, src, n);
  memset(dst + n, 0, cap - n);
}
}  // namespace

const char *tracker_role_name(uint32_t role) { return role < SVR_TRK_ROLE_COUNT ? kRoles[role].name : ""; }
const char *tracker_role_setting(uint32_t role) {
  return role < SVR_TRK_ROLE_COUNT ? kRoles[role].setting : kRoles[0].setting;
}
uint32_t tracker_role_from_name(const char *name) {
  if (!name || !*name) return SVR_TRK_ROLE_NONE;
  if (!strncmp(name, "vive_tracker_", 13)) name += 13;
  for (uint32_t r = 1; r < SVR_TRK_ROLE_COUNT; r++) {
    if (iequal(name, kRoles[r].name) || iequal(name, kRoles[r].setting)) return r;
    if (!strncmp(name, "TrackerRole_", 12) && snake(name + 12) == kRoles[r].name) return r;
  }
  return SVR_TRK_ROLE_NONE;
}

std::string TrackerTable::default_path() {
  const char *p = getenv("STEAMAC_VR_TRACKERS");
  return p && *p ? p : SVR_TRK_DEFAULT_PATH;
}

bool TrackerTable::open(const char *path, bool create) {
  close();
  std::string p = path && *path ? path : default_path();
  const size_t size = sizeof(svr_trk_table);
  void *m = nullptr;
#ifdef _WIN32
  // a Unix path names the same file through Wine's Z: drive
  std::string w = p;
  if (!w.empty() && w[0] == '/') {
    w = "Z:" + w;
    for (char &c : w)
      if (c == '/') c = '\\';
  }
  HANDLE f = CreateFileA(w.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER cur;
  if (!GetFileSizeEx(f, &cur) || (uint64_t)cur.QuadPart < size) {
    if (!create) {
      CloseHandle(f);
      return false;
    }
  }
  HANDLE sec = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, (DWORD)size, nullptr);  // grows the file if needed
  CloseHandle(f);
  if (!sec) return false;
  m = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!m) {
    CloseHandle(sec);
    return false;
  }
  handle_ = sec;
#else
  int fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0666);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) || ((size_t)st.st_size < size && (!create || ftruncate(fd, (off_t)size)))) {
    ::close(fd);
    return false;
  }
  if (create) fchmod(fd, 0666);  // sources in other users' or Wine's processes may join
  m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) return false;
#endif
  t_ = (svr_trk_table *)m;
  // an empty (zero) file: initialise the header; the magic is stored last
  if (load_acq(&t_->magic) == 0) {
    t_->version = SVR_TRK_VERSION;
    t_->size = (uint32_t)size;
    t_->max = SVR_TRK_MAX;
    uint32_t zero = 0;
    __atomic_compare_exchange_n(&t_->magic, &zero, SVR_TRK_MAGIC, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  if (load_acq(&t_->magic) != SVR_TRK_MAGIC || t_->version != SVR_TRK_VERSION || t_->size != size ||
      t_->max != SVR_TRK_MAX) {
    close();
    return false;
  }
  return true;
}

void TrackerTable::close() {
  if (!t_) return;
#ifdef _WIN32
  UnmapViewOfFile(t_);
  CloseHandle((HANDLE)handle_);
  handle_ = nullptr;
#else
  munmap(t_, sizeof(svr_trk_table));
#endif
  t_ = nullptr;
}

int TrackerTable::claim(const svr_trk_device &desc, bool present) {
  if (!t_ || !desc.serial[0]) return -1;
  int slot = -1;
  // a restarted source takes over its old slot (same serial); otherwise the first free one
  for (int i = 0; i < SVR_TRK_MAX && slot < 0; i++) {
    svr_trk_device &d = t_->dev[i];
    if (load_acq(&d.state) != SVR_TRK_FREE && !strncmp(d.serial, desc.serial, sizeof d.serial)) {
      __atomic_store_n(&d.state, (uint32_t)SVR_TRK_CLAIMED, __ATOMIC_RELEASE);
      slot = i;
    }
  }
  for (int i = 0; i < SVR_TRK_MAX && slot < 0; i++) {
    uint32_t want = SVR_TRK_FREE;
    if (__atomic_compare_exchange_n(&t_->dev[i].state, &want, (uint32_t)SVR_TRK_CLAIMED, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED))
      slot = i;
  }
  if (slot < 0) return -1;
  svr_trk_device &d = t_->dev[slot];
  seq_begin(&d.seq);
  d.device_class = desc.device_class ? desc.device_class : 3 /* TrackedDeviceClass_GenericTracker */;
  d.role = desc.role < SVR_TRK_ROLE_COUNT ? desc.role : (uint32_t)SVR_TRK_ROLE_NONE;
  d.updated = desc.updated;
  memset(&d.pose, 0, sizeof d.pose);
  d.pose.orientation[3] = 1;
  d.writer_pid = desc.writer_pid;
  copy_str(d.serial, sizeof d.serial, desc.serial);
  copy_str(d.model, sizeof d.model, desc.model);
  copy_str(d.manufacturer, sizeof d.manufacturer, desc.manufacturer);
  copy_str(d.tracking_system, sizeof d.tracking_system, desc.tracking_system);
  copy_str(d.controller_type, sizeof d.controller_type, desc.controller_type);
  copy_str(d.render_model, sizeof d.render_model, desc.render_model);
  copy_str(d.input_profile, sizeof d.input_profile, desc.input_profile);
  seq_end(&d.seq);
  if (present) __atomic_store_n(&d.state, (uint32_t)SVR_TRK_PRESENT, __ATOMIC_RELEASE);
  return slot;
}

void TrackerTable::update(int slot, const svr_pose &pose, int64_t writer_now_ns) {
  if (!t_ || slot < 0 || slot >= SVR_TRK_MAX) return;
  svr_trk_device &d = t_->dev[slot];
  seq_begin(&d.seq);
  d.pose = pose;
  d.updated = writer_now_ns;
  seq_end(&d.seq);
}

void TrackerTable::release(int slot) {
  if (!t_ || slot < 0 || slot >= SVR_TRK_MAX) return;
  svr_trk_device &d = t_->dev[slot];
  seq_begin(&d.seq);
  d.serial[0] = 0;
  seq_end(&d.seq);
  __atomic_store_n(&d.state, (uint32_t)SVR_TRK_FREE, __ATOMIC_RELEASE);
}

void TrackerTable::write_mirror(const svr_trk_mirror &m) {
  if (!t_) return;
  svr_trk_mirror &dst = t_->mirror;
  seq_begin(&dst.seq);
  uint32_t seq = dst.seq;
  memcpy((void *)&dst, (const void *)&m, sizeof m);
  dst.seq = seq;
  seq_end(&dst.seq);
}

bool TrackerTable::read_device(int slot, svr_trk_device *out) const {
  if (!t_ || slot < 0 || slot >= SVR_TRK_MAX) return false;
  const svr_trk_device &d = t_->dev[slot];
  if (load_acq(&d.state) != SVR_TRK_PRESENT) return false;
  if (!seq_read(&d, &d.seq, out)) return false;
  return load_acq(&d.state) == SVR_TRK_PRESENT && out->serial[0];
}

uint32_t TrackerTable::read_mirror(svr_trk_mirror *out) const {
  if (!t_ || !load_acq(&t_->mirror.seq)) return 0;
  return seq_read(&t_->mirror, &t_->mirror.seq, out);
}

uint32_t TrackerView::refresh(const TrackerTable &t, int64_t now) {
  uint32_t changed = 0;
  for (int i = 0; i < SVR_TRK_MAX; i++) {
    Dev &v = dev[i];
    svr_trk_device d;
    bool present = t.read_device(i, &d);
    bool was = v.connected;
    if (!present) {
      v.present = v.connected = false;
    } else {
      if (!v.present || strncmp(v.d.serial, d.serial, sizeof d.serial)) {
        // first sight: connected only once the sequence moves (a crashed source's slot never does)
        v.seq = d.seq;
        v.seen = now - SVR_TRK_STALE_NS;
      } else if (d.seq != v.seq) {
        v.seq = d.seq;
        v.seen = now;
      }
      v.present = true;
      v.d = d;
      v.connected = now - v.seen < SVR_TRK_STALE_NS;
    }
    if (v.connected != was) changed |= 1u << i;
  }
  return changed;
}

bool TrackerView::mirror_live(const TrackerTable &t, int64_t now, svr_trk_mirror *out) {
  uint32_t s = t.read_mirror(out);
  if (!s) return false;
  if (s != mirror_seq) {
    // first sight counts as stale: a mirror left by a scene app that has exited never moves again
    mirror_seen = mirror_seq ? now : now - SVR_TRK_STALE_NS;
    mirror_seq = s;
  }
  return now - mirror_seen < SVR_TRK_STALE_NS;
}
}  // namespace svr
