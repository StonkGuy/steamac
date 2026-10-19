/* SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later
 *
 * steamac VR guest link: the messages between the guest OpenVR runtime (vrclient.so, x86-64 Linux under FEX)
 * and the host (steamac-vm's wmtr executor, which relays them to steamac-vrhost; or vr-fakehost in tests).
 *
 * Every message is one fixed-size little-endian struct starting with struct svr_hdr. A request gets exactly one
 * reply. Transports (runtime/transport.cpp):
 *   wmtr   one synchronous wmtr request (call WMTR_CALL_VR) on the game process's own wmtr context, so texture
 *          handles in SVR_SUBMIT resolve in the context that created them (docs/vr-interfaces.md section 4).
 *   socket SOCK_SEQPACKET Unix socket, STEAMAC_VR_SOCKET=<path> (vr-fakehost, tests).
 *
 * Times: the host monotonic clock in ns. Each reply carries host_now so the guest can convert to its own clock
 * (one-way latency of the ring is ignored: it is microseconds against a 11-14 ms frame). Host calls never block
 * for a frame: SVR_FRAME returns at once and the guest sleeps until wake_up itself, so the single wmtr executor
 * thread is never held by VR pacing. */
#ifndef STEAMAC_VR_GUEST_H
#define STEAMAC_VR_GUEST_H
#include <stdint.h>

#define SVR_VERSION 1

enum svr_op {
  SVR_HELLO = 1,  /* svr_hello_req   -> svr_hello_rep */
  SVR_FRAME = 2,  /* svr_frame_req   -> svr_frame_rep: timing + poses + input snapshot */
  SVR_SUBMIT = 3, /* svr_submit_req  -> svr_ack: both eyes of one frame */
  SVR_HAPTIC = 4, /* svr_haptic_req  -> svr_ack */
};

struct svr_hdr {
  uint32_t op;   /* enum svr_op */
  uint32_t size; /* bytes of the whole struct */
};

struct svr_ack {
  struct svr_hdr h;
  int32_t status; /* 0 = ok, negative errno otherwise */
  uint32_t pad;
};

/* tracked poses; matches vrhost::Device (docs/vr-interfaces.md section 2) */
enum svr_device { SVR_DEV_HEAD, SVR_DEV_LEFT_GRIP, SVR_DEV_LEFT_AIM, SVR_DEV_RIGHT_GRIP, SVR_DEV_RIGHT_AIM, SVR_DEV_COUNT };

#define SVR_POSE_VALID 1u   /* orientation and position are valid */
#define SVR_POSE_TRACKED 2u /* currently tracked (not inferred) */
#define SVR_POSE_VEL 4u     /* lin_vel/ang_vel valid */

struct svr_pose {
  float orientation[4]; /* x, y, z, w */
  float position[3];    /* metres, stage space (+y up, -z forward), OpenXR/OpenVR convention */
  float lin_vel[3];     /* m/s, stage space */
  float ang_vel[3];     /* rad/s, stage space */
  uint32_t flags;       /* SVR_POSE_* */
};

/* controller family the headset reports; selects the OpenVR controller type (binding file) */
enum svr_profile {
  SVR_PROFILE_NONE = 0,
  SVR_PROFILE_TOUCH = 1, /* Quest/Pico/Touch-style: oculus_touch bindings */
  SVR_PROFILE_INDEX = 2, /* Index-style: knuckles bindings */
};

/* adapter-neutral input ids, one table per hand: input[hand][id], hand 0 = left, 1 = right.
 * Buttons are 0/1, analog values 0..1, axes -1..1. */
enum svr_input {
  SVR_IN_TRIGGER_VALUE,
  SVR_IN_TRIGGER_CLICK,
  SVR_IN_TRIGGER_TOUCH,
  SVR_IN_SQUEEZE_VALUE,
  SVR_IN_SQUEEZE_CLICK,
  SVR_IN_SQUEEZE_FORCE, /* Index grip force */
  SVR_IN_THUMBSTICK_X,
  SVR_IN_THUMBSTICK_Y,
  SVR_IN_THUMBSTICK_CLICK,
  SVR_IN_THUMBSTICK_TOUCH,
  SVR_IN_A_CLICK, /* left hand: X */
  SVR_IN_A_TOUCH,
  SVR_IN_B_CLICK, /* left hand: Y */
  SVR_IN_B_TOUCH,
  SVR_IN_MENU_CLICK, /* left: menu; right: system (reserved by the headset on Quest) */
  SVR_IN_SYSTEM_CLICK,
  SVR_IN_THUMBREST_TOUCH,
  SVR_IN_TRACKPAD_X, /* Index */
  SVR_IN_TRACKPAD_Y,
  SVR_IN_TRACKPAD_TOUCH,
  SVR_IN_TRACKPAD_FORCE,
  SVR_IN_COUNT
};

struct svr_hello_req {
  struct svr_hdr h;
  uint32_t version; /* SVR_VERSION */
  uint32_t pid;
  char app_key[64]; /* OpenVR app key or executable name, for host logs */
};

struct svr_hello_rep {
  struct svr_hdr h;
  uint32_t version;
  uint32_t connected; /* a headset is connected */
  uint32_t eye_width, eye_height;
  float refresh_hz;
  float ipd_m;
  float fov[2][4]; /* per eye: left, right, up, down in radians (OpenXR: left/down negative) */
  uint32_t profile; /* enum svr_profile */
  uint32_t pad;
  char name[64]; /* headset model, e.g. "Quest 3" */
};

struct svr_frame_req {
  struct svr_hdr h;
  uint64_t last_frame_id; /* frame id the guest last got (0 at start) */
};

struct svr_frame_rep {
  struct svr_hdr h;
  uint64_t frame_id;          /* next frame to render */
  int64_t host_now;           /* host clock when the reply was built */
  int64_t wake_up;            /* when the app should start the frame (WaitGetPoses returns) */
  int64_t predicted_display;  /* when the frame will be shown */
  int64_t pose_time;          /* the time `pose` is for (normally predicted_display); the guest extrapolates */
  float refresh_hz;
  uint32_t connected;
  uint32_t profile;
  uint32_t pad;
  float ipd_m;
  float fov[2][4];
  struct svr_pose pose[SVR_DEV_COUNT];
  float input[2][SVR_IN_COUNT];
};

/* A frame submit: exactly DXMT's struct WMTVRSubmitInfo (winemetal.h, StonkGuy/dxmt vr-export), which the wmtr
 * transport hands to winemetal call 146 (WMTVRSubmit) unchanged; the socket transport sends it as SVR_SUBMIT. */
#define SVR_EYE_FLIP_Y 1u /* = WMTVR_EYE_FLIP_Y: source rows are bottom-up */
#define SVR_EYE_SRGB 2u   /* = WMTVR_EYE_SRGB: data is gamma encoded in a UNORM texture */
struct svr_wmtvr_eye {
  uint64_t texture;    /* MTLTexture handle (IMTLD3D11ExportableTexture::GetMetalTexture) */
  uint32_t x, y, w, h; /* sub-rect in texels; w == 0 means the whole texture */
  uint32_t flags;      /* SVR_EYE_* */
  uint32_t slice;      /* array slice (single-pass stereo) */
};
struct svr_wmtvr_submit {
  uint64_t frame_id;
  uint64_t display_time; /* host clock: the predicted_display the frame was rendered for */
  struct svr_wmtvr_eye eye[2];
  float pose[2][7]; /* per eye: orientation x,y,z,w, position x,y,z (stage space) */
  float fov[2][4];  /* per eye: left, right, up, down in radians */
  uint64_t wait_event; /* MTLSharedEvent the export waits on (0: none) */
  uint64_t wait_value;
};
#define SVR_WMTVR_SUBMIT_CALL 146 /* winemetal unix call index of WMTVRSubmit */

struct svr_submit_req {
  struct svr_hdr h;
  struct svr_wmtvr_submit info;
};

struct svr_haptic_req {
  struct svr_hdr h;
  uint32_t hand; /* 0 left, 1 right */
  float duration_s, frequency, amplitude;
};

/* The texture description vrclient_x64.dll (Proton patch guest/proton-vrclient) passes as Texture_t.handle with
 * eType = TextureType_Metal for a DXMT texture: handle, event and value from DXMT's
 * IMTLD3D11ExportableTexture::GetMetalTexture, the rest from D3D11_TEXTURE2D_DESC. */
#define SVR_METAL_TEXTURE_MAGIC 0x54525653u /* "SVRT" */
struct svr_metal_texture {
  uint32_t magic; /* SVR_METAL_TEXTURE_MAGIC */
  uint32_t size;  /* sizeof(struct svr_metal_texture) */
  uint64_t handle;       /* MTLTexture */
  uint64_t event_handle; /* MTLSharedEvent; the texture is complete when it reaches event_value */
  uint64_t event_value;
  uint32_t width, height;
  uint32_t dxgi_format; /* DXGI_FORMAT of the D3D11 texture */
  uint32_t array_size, sample_count;
  uint32_t array_index; /* slice for this eye (SubmitWithArrayIndex; else the eye for an array texture, 0 otherwise) */
};

#endif
