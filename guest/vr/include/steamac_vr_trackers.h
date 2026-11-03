/* SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later
 *
 * steamac VR tracked-device table: how extra tracked devices (full-body trackers, pucks, estimated body points)
 * reach the guest OpenVR runtime, and how processes without a host link (companion apps, driver hosts) read the
 * headset's and controllers' poses. docs/vr-trackers.md describes the device model and how to write a source.
 *
 * The table is one shared file mapping (default /dev/shm/steamac-vr-trackers, STEAMAC_VR_TRACKERS=<path> to move
 * it). Linux processes mmap it; Windows processes under Wine/Proton map the same file through its Z: drive path
 * (CreateFileMapping on Z:\dev\shm\steamac-vr-trackers), so a Windows SteamVR driver hosted in a Wine process feeds
 * the same table as a native source.
 *
 * Layout: struct svr_trk_table, little-endian, fixed size. No shared clock is needed: readers time a device by
 * when they see its sequence number change (a device whose sequence has not moved for SVR_TRK_STALE_NS reads as
 * disconnected), and extrapolate from there with the device's velocities.
 *
 * Writing a device (one writer per slot; slots are claimed with an atomic compare-and-swap on `state`):
 *   1. claim: find the slot whose serial is yours (a restart reuses it) or CAS a free slot SVR_TRK_FREE ->
 *      SVR_TRK_CLAIMED; fill class, role and the strings; then store SVR_TRK_PRESENT.
 *   2. every pose update: seq++ (odd = writing), write pose/updated, seq++ (even again). Readers retry on odd or
 *      changed seq (seqlock). Update at least every 100 ms even if the pose does not change.
 *   3. remove: store SVR_TRK_FREE (the runtime sends TrackedDeviceDeactivated).
 * steamac's helpers (guest/vr/runtime/trackers.h) implement all of this; other languages follow the layout. */
#ifndef STEAMAC_VR_TRACKERS_H
#define STEAMAC_VR_TRACKERS_H
#include <stdint.h>

#include "steamac_vr_guest.h"

#define SVR_TRK_MAGIC 0x4B525456u /* "VTRK" */
#define SVR_TRK_VERSION 1u
#define SVR_TRK_MAX 16 /* OpenVR device indices 3 .. 3 + SVR_TRK_MAX - 1 */
#define SVR_TRK_DEFAULT_PATH "/dev/shm/steamac-vr-trackers"
#define SVR_TRK_STALE_NS 2000000000LL /* no sequence change for 2 s: disconnected (tolerates source stalls under emulation load) */

enum svr_trk_state { SVR_TRK_FREE = 0, SVR_TRK_CLAIMED = 1, SVR_TRK_PRESENT = 2 };

/* SteamVR tracker roles (steamvr.vrsettings "trackers" section, "TrackerRole_*"); the runtime maps a role to
 * Prop_ControllerType_String "vive_tracker_<role>" unless the device sets its own controller type. */
enum svr_trk_role {
  SVR_TRK_ROLE_NONE = 0,
  SVR_TRK_ROLE_HANDED,
  SVR_TRK_ROLE_LEFT_FOOT,
  SVR_TRK_ROLE_RIGHT_FOOT,
  SVR_TRK_ROLE_LEFT_SHOULDER,
  SVR_TRK_ROLE_RIGHT_SHOULDER,
  SVR_TRK_ROLE_LEFT_ELBOW,
  SVR_TRK_ROLE_RIGHT_ELBOW,
  SVR_TRK_ROLE_LEFT_KNEE,
  SVR_TRK_ROLE_RIGHT_KNEE,
  SVR_TRK_ROLE_WAIST,
  SVR_TRK_ROLE_CHEST,
  SVR_TRK_ROLE_CAMERA,
  SVR_TRK_ROLE_KEYBOARD,
  SVR_TRK_ROLE_LEFT_WRIST,
  SVR_TRK_ROLE_RIGHT_WRIST,
  SVR_TRK_ROLE_LEFT_ANKLE,
  SVR_TRK_ROLE_RIGHT_ANKLE,
  SVR_TRK_ROLE_COUNT
};

struct svr_trk_device {
  uint32_t seq;          /* seqlock over pose/updated: odd while a write is in progress */
  uint32_t state;        /* enum svr_trk_state */
  uint32_t device_class; /* ETrackedDeviceClass: 3 = GenericTracker (default), 2 = Controller, 4 = TrackingReference */
  uint32_t role;         /* enum svr_trk_role */
  int64_t updated;       /* writer's own monotonic clock in ns (informational only) */
  struct svr_pose pose;  /* stage space, the same space as the headset's poses (+y up, -z forward, metres) */
  uint32_t writer_pid;   /* informational */
  uint32_t pad;
  /* NUL-terminated OpenVR string properties; empty = the runtime's default for the class and role */
  char serial[48];           /* Prop_SerialNumber_String: identity of the device, unique in the table */
  char model[48];            /* Prop_ModelNumber_String */
  char manufacturer[48];     /* Prop_ManufacturerName_String */
  char tracking_system[32];  /* Prop_TrackingSystemName_String */
  char controller_type[48];  /* Prop_ControllerType_String */
  char render_model[64];     /* Prop_RenderModelName_String */
  char input_profile[96];    /* Prop_InputProfilePath_String */
};

/* the headset and controllers as the scene app's runtime last saw them; written by the process that has the host
 * link (the game), read by processes without one */
struct svr_trk_mirror {
  uint32_t seq; /* seqlock */
  uint32_t connected;
  uint32_t profile; /* enum svr_profile */
  float refresh_hz;
  float ipd_m;
  float fov[2][4];
  uint32_t eye_width, eye_height;
  uint32_t pad;
  struct svr_pose pose[SVR_DEV_COUNT];
  float input[2][SVR_IN_COUNT];
};

struct svr_trk_table {
  uint32_t magic;   /* SVR_TRK_MAGIC */
  uint32_t version; /* SVR_TRK_VERSION */
  uint32_t size;    /* sizeof(struct svr_trk_table) */
  uint32_t max;     /* SVR_TRK_MAX */
  struct svr_trk_mirror mirror;
  struct svr_trk_device dev[SVR_TRK_MAX];
};

#endif
