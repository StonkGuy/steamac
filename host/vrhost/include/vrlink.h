/* SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later */
/* VR link between steamac-vm (wmtr executor, Apache-2.0) and steamac-vrhost (GPL-3.0-or-later).
 * Contract: docs/vr-interfaces.md section 3. Plain C structs only, so both sides can include this header
 * without sharing any other code.
 *
 * Channel 1: AF_UNIX SOCK_DGRAM socketpair (macOS has no AF_UNIX SOCK_SEQPACKET; datagrams on a local
 *            socketpair are reliable, ordered and keep message boundaries). steamac-vm passes one end as
 *            --link-fd=N; for tests/manual runs a listener at a path hands one end over by SCM_RIGHTS.
 *            One struct per datagram, header first, little-endian, no pointers, each < 2048 bytes
 *            (net.local.dgram.maxdgram).
 * Channel 2: Mach. steamac-vrhost registers a receive right under a bootstrap name (--mach-name=...);
 *            steamac-vm looks it up and sends one struct vrl_mach_swapchain per swapchain, carrying the
 *            IOSurface send rights (IOSurfaceCreateMachPort) of its images. Never per frame.
 *
 * Times are host monotonic nanoseconds: clock_gettime_nsec_np(CLOCK_UPTIME_RAW). */
#ifndef VRLINK_H
#define VRLINK_H
#include <stdint.h>
#ifdef __APPLE__
#include <mach/message.h>
#endif

#define VRL_VERSION 1
#define VRL_MAX_IMAGES 4 /* images per swapchain */

enum vrl_type {
  VRL_HELLO = 1,     /* both directions, first packet: struct vrl_hello */
  VRL_SWAPCHAIN = 2, /* vm -> host: struct vrl_swapchain (ports follow on the Mach channel) */
  VRL_SUBMIT = 3,    /* vm -> host: struct vrl_submit */
  VRL_RELEASE = 4,   /* host -> vm: struct vrl_release */
  VRL_TIMING = 5,    /* host -> vm: struct vrl_timing, once per frame (pacer) */
  VRL_POSES = 6,     /* host -> vm: struct vrl_poses, once per frame and on new tracking data */
  VRL_INPUT = 7,     /* host -> vm: struct vrl_input, on change */
  VRL_HAPTICS = 8,   /* vm -> host: struct vrl_haptics */
  VRL_STATE = 9,     /* host -> vm: struct vrl_state, on connect/disconnect/refresh-rate change */
};

/* SO_RCVBUF both ends set on the link socket (the macOS default for AF_UNIX datagrams is 4 KiB) */
#define VRL_SOCKET_BUFFER (256 * 1024)

struct vrl_header {
  uint32_t type; /* enum vrl_type */
  uint32_t size; /* bytes of the whole packet */
};

struct vrl_hello {
  struct vrl_header h;
  uint32_t version; /* VRL_VERSION */
  uint32_t pid;
};

#define VRL_SWAPCHAIN_SRGB 1u /* image data is gamma encoded; the host samples it through an sRGB view */

/* A set of IOSurface-backed images owned by steamac-vm. Every image has the same size and format. */
struct vrl_swapchain {
  struct vrl_header h;
  uint32_t id;           /* nonzero, unique per vm link; a new VRL_SWAPCHAIN with the same id replaces it */
  uint32_t image_count;  /* <= VRL_MAX_IMAGES */
  uint32_t width, height;
  uint32_t pixel_format; /* MTLPixelFormat of the images (RGBA8Unorm, BGRA8Unorm (+_sRGB), RGBA16Float) */
  uint32_t flags;        /* VRL_SWAPCHAIN_* */
};

#define VRL_EYE_FLIP_Y 1u /* rows are bottom-up */

struct vrl_eye {
  uint32_t swapchain, image;
  uint32_t x, y, w, h; /* sub-rect of the image that holds this eye */
  uint32_t flags;      /* VRL_EYE_* */
  uint32_t pad;
};

/* The images named here are complete (GPU writes finished) when this packet is sent. They stay owned by the
 * host until it sends VRL_RELEASE for them. */
struct vrl_submit {
  struct vrl_header h;
  uint64_t frame_id;
  int64_t display_time; /* predicted display time the app rendered for */
  int64_t app_submit_time; /* when the guest's submit reached the vm side */
  int64_t ready_time;      /* when the copy into the swapchain image completed (packet send time) */
  struct vrl_eye eye[2];
  float pose[2][7]; /* per eye: orientation x,y,z,w, position x,y,z; stage space */
  float fov[2][4];  /* per eye: left, right, up, down; radians, left/down negative */
};

struct vrl_release {
  struct vrl_header h;
  uint32_t swapchain, image;
  uint64_t frame_id;     /* the frame that used the image */
  int64_t release_time;  /* host time the image was consumed (compositor pass complete) */
};

/* --- tracking, input and timing for the guest OpenVR runtime (M2/M3). The vm side keeps the newest of each
 * and answers the guest's polls from that copy (wmtr call 147), so none of these is ever waited for. --- */

/* matches vrhost::Device and the guest's svr_device */
enum vrl_device { VRL_DEV_HEAD, VRL_DEV_LEFT_GRIP, VRL_DEV_LEFT_AIM, VRL_DEV_RIGHT_GRIP, VRL_DEV_RIGHT_AIM, VRL_DEV_COUNT };
#define VRL_POSE_VALID 1u
#define VRL_POSE_TRACKED 2u
#define VRL_POSE_VEL 4u
struct vrl_pose {
  float orientation[4]; /* x, y, z, w */
  float position[3];    /* stage space, metres */
  float lin_vel[3];     /* stage space, m/s */
  float ang_vel[3];     /* stage space, rad/s */
  uint32_t flags;       /* VRL_POSE_* */
};

enum vrl_profile { VRL_PROFILE_NONE, VRL_PROFILE_TOUCH, VRL_PROFILE_INDEX }; /* controller family */

struct vrl_state {
  struct vrl_header h;
  uint32_t connected;
  uint32_t eye_width, eye_height; /* recommended render size per eye */
  float refresh_hz;
  float ipd_m;
  float fov[2][4]; /* per eye: left, right, up, down; radians */
  uint32_t profile; /* enum vrl_profile */
  uint32_t pad;
  char name[64];    /* headset model */
};

/* the next frame the app should render (the pacer's prediction) */
struct vrl_timing {
  struct vrl_header h;
  uint64_t frame_id;
  int64_t wake_up;           /* when the app should start it (WaitGetPoses returns) */
  int64_t predicted_display; /* when it will be shown */
  float refresh_hz;
  uint32_t render_divisor; /* the pacer releases a frame every this many display periods (spacewarp: 2); 0 = 1 */
};

struct vrl_poses {
  struct vrl_header h;
  int64_t time; /* the time the poses are for (predicted_display of the current timing, or sample time) */
  struct vrl_pose pose[VRL_DEV_COUNT];
};

#define VRL_INPUT_COUNT 21 /* = guest SVR_IN_COUNT; ids in guest/vr/include/steamac_vr_guest.h (enum svr_input) */
struct vrl_input {
  struct vrl_header h;
  int64_t time;
  uint32_t profile; /* enum vrl_profile */
  uint32_t pad;
  float value[2][VRL_INPUT_COUNT]; /* [hand 0 left, 1 right][svr_input id] */
};

struct vrl_haptics {
  struct vrl_header h;
  uint32_t hand; /* 0 left, 1 right */
  float duration_s, frequency, amplitude; /* frequency 0 = device default */
};

#ifdef __APPLE__
/* Mach channel message: ports[i] is a send right for image i (IOSurfaceCreateMachPort). The receiver
 * takes ownership of the rights. msgh_id = VRL_MACH_SWAPCHAIN_ID. */
#define VRL_MACH_SWAPCHAIN_ID 0x56524c31 /* "VRL1" */
struct vrl_mach_swapchain {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t ports[VRL_MAX_IMAGES];
  uint32_t swapchain_id;
  uint32_t image_count;
};
struct vrl_mach_swapchain_rcv {
  struct vrl_mach_swapchain msg;
  mach_msg_trailer_t trailer;
};
#endif

#endif
