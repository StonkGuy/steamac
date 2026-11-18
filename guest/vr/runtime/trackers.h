// SPDX-License-Identifier: Apache-2.0
// The tracked-device table (include/steamac_vr_trackers.h): mapping, slot claim, seqlocked writes and reads, and the
// reader-side view that times devices without a shared clock. Used by the runtime, svr-driverhost (Linux and
// Windows builds) and svr-tracker-feed; depends on nothing but the C library (and kernel32 on Windows).
#pragma once
#include <cstdint>
#include <string>

#include "steamac_vr_trackers.h"

namespace svr {
// "waist" for SVR_TRK_ROLE_WAIST (the suffix of Prop_ControllerType_String "vive_tracker_waist"); "" for none
const char *tracker_role_name(uint32_t role);
// "TrackerRole_Waist" (steamvr.vrsettings "trackers" values)
const char *tracker_role_setting(uint32_t role);
// accepts "waist", "TrackerRole_Waist", "vive_tracker_waist" (any case); SVR_TRK_ROLE_NONE if unknown
uint32_t tracker_role_from_name(const char *name);

class TrackerTable {
 public:
  TrackerTable() = default;
  ~TrackerTable() { close(); }
  TrackerTable(const TrackerTable &) = delete;
  TrackerTable &operator=(const TrackerTable &) = delete;

  // STEAMAC_VR_TRACKERS, else SVR_TRK_DEFAULT_PATH
  static std::string default_path();
  // Maps the table (creating the file when `create`); initialises an empty file. False if absent or incompatible.
  bool open(const char *path = nullptr, bool create = false);
  void close();
  bool is_open() const { return t_ != nullptr; }
  svr_trk_table *raw() const { return t_; }

  // --- writer ---
  // claims a slot for `desc` (by serial: an existing slot with the same serial is taken over) and publishes it
  // with an invalid pose; returns the slot or -1 when the table is full. present = false only reserves the slot
  // (readers do not see it) until a later claim with the same serial publishes it.
  int claim(const svr_trk_device &desc, bool present = true);
  void update(int slot, const svr_pose &pose, int64_t writer_now_ns);
  void release(int slot);
  void write_mirror(const svr_trk_mirror &m);

  // --- reader ---
  // consistent copy of a present device; false if the slot is not SVR_TRK_PRESENT
  bool read_device(int slot, svr_trk_device *out) const;
  // consistent copy of the mirror; returns its sequence number (0: never written)
  uint32_t read_mirror(svr_trk_mirror *out) const;

 private:
  svr_trk_table *t_ = nullptr;
  void *handle_ = nullptr;  // Windows: the section handle
};

// What one reader knows about the table's devices: a device is connected while its sequence number keeps changing
// (at least every SVR_TRK_STALE_NS; a device seen for the first time connects on its next update); its pose is
// taken to be current when the reader first saw that sequence.
struct TrackerView {
  struct Dev {
    bool present = false, connected = false;
    uint32_t seq = 0;
    int64_t seen = 0;  // reader clock when `seq` was first seen
    svr_trk_device d{};
  };
  Dev dev[SVR_TRK_MAX];
  uint32_t mirror_seq = 0;
  int64_t mirror_seen = 0;

  // Re-reads every slot at reader time `now`; returns a bit mask of the slots whose connected state changed.
  uint32_t refresh(const TrackerTable &t, int64_t now);
  // the mirror if its sequence moved within SVR_TRK_STALE_NS (a scene app with a host link is running)
  bool mirror_live(const TrackerTable &t, int64_t now, svr_trk_mirror *out);
};
}  // namespace svr
