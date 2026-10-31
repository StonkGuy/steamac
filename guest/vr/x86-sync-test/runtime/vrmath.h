// SPDX-License-Identifier: Apache-2.0
// Pose math shared by the runtime and its unit tests.
#pragma once
#include <cmath>

#include "openvr.h"
#include "steamac_vr_guest.h"

namespace svr {
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

inline Quat qmul(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat qnorm(Quat q) {
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-12f) return Quat{};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// rotate v by q
inline void qrot(Quat q, const float v[3], float out[3]) {
  // t = 2 * cross(q.xyz, v); out = v + w*t + cross(q.xyz, t)
  float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
  out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
  out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
  out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

// Advance a pose by dt seconds with its (stage-space) linear and angular velocity.
inline svr_pose extrapolate(const svr_pose &p, float dt) {
  svr_pose r = p;
  if (!(p.flags & SVR_POSE_VEL) || dt == 0) return r;
  for (int i = 0; i < 3; i++) r.position[i] = p.position[i] + p.lin_vel[i] * dt;
  float wx = p.ang_vel[0], wy = p.ang_vel[1], wz = p.ang_vel[2];
  float angle = std::sqrt(wx * wx + wy * wy + wz * wz) * dt;
  if (angle > 1e-9f) {
    float s = std::sin(angle / 2) / (angle / dt);
    Quat dq{wx * s, wy * s, wz * s, std::cos(angle / 2)};
    Quat q{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
    q = qnorm(qmul(dq, q));  // world-space angular velocity: pre-multiply
    r.orientation[0] = q.x, r.orientation[1] = q.y, r.orientation[2] = q.z, r.orientation[3] = q.w;
  }
  return r;
}

inline vr::HmdMatrix34_t to_matrix34(const float q4[4], const float pos[3]) {
  float x = q4[0], y = q4[1], z = q4[2], w = q4[3];
  vr::HmdMatrix34_t m;
  m.m[0][0] = 1 - 2 * (y * y + z * z), m.m[0][1] = 2 * (x * y - z * w), m.m[0][2] = 2 * (x * z + y * w);
  m.m[1][0] = 2 * (x * y + z * w), m.m[1][1] = 1 - 2 * (x * x + z * z), m.m[1][2] = 2 * (y * z - x * w);
  m.m[2][0] = 2 * (x * z - y * w), m.m[2][1] = 2 * (y * z + x * w), m.m[2][2] = 1 - 2 * (x * x + y * y);
  m.m[0][3] = pos[0], m.m[1][3] = pos[1], m.m[2][3] = pos[2];
  return m;
}

inline vr::HmdMatrix34_t identity34() {
  static const float q[4] = {0, 0, 0, 1}, p[3] = {0, 0, 0};
  return to_matrix34(q, p);
}

inline vr::TrackedDevicePose_t to_openvr(const svr_pose &p, bool connected) {
  vr::TrackedDevicePose_t t{};
  t.bDeviceIsConnected = connected;
  t.bPoseIsValid = connected && (p.flags & SVR_POSE_VALID);
  t.eTrackingResult = !connected ? vr::TrackingResult_Uninitialized
                      : (p.flags & SVR_POSE_TRACKED) ? vr::TrackingResult_Running_OK
                                                     : vr::TrackingResult_Running_OutOfRange;
  t.mDeviceToAbsoluteTracking = t.bPoseIsValid ? to_matrix34(p.orientation, p.position) : identity34();
  for (int i = 0; i < 3; i++) {
    t.vVelocity.v[i] = p.lin_vel[i];
    t.vAngularVelocity.v[i] = p.ang_vel[i];
  }
  return t;
}

// OpenVR projection from raw tangents (GetProjectionRaw values), D3D convention as SteamVR returns it.
inline vr::HmdMatrix44_t projection(float l, float r, float t, float b, float zn, float zf) {
  vr::HmdMatrix44_t m{};
  float idx = 1.0f / (r - l), idy = 1.0f / (b - t), idz = 1.0f / (zf - zn);
  float sx = r + l, sy = b + t;
  m.m[0][0] = 2 * idx, m.m[0][2] = sx * idx;
  m.m[1][1] = 2 * idy, m.m[1][2] = sy * idy;
  m.m[2][2] = -zf * idz, m.m[2][3] = -zf * zn * idz;
  m.m[3][2] = -1.0f;
  return m;
}
}  // namespace svr
