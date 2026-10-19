// SPDX-License-Identifier: Apache-2.0
// Hand skeleton from finger curl and splay. A controller reports no joint angles, so the pose is a simulation:
// each finger is a chain of fixed-length bones whose joints bend with the curl value, the way OpenVR's
// handskeletonsimulation driver sample does it (ValveSoftware/openvr samples/drivers, BSD-3-Clause, Copyright (c)
// Valve Corporation; itself after Monado's u_hand_simulation.c). The joint lengths, rest angles, curl/splay
// gains, the wrist offset and the left-to-right mirroring are that sample's; see third_party/openvr/LICENSE.
#include "skeleton.h"

#include <cmath>

#include "vrmath.h"

namespace svr {
namespace {
const float kDeg = 3.14159265358979f / 180.0f;

// bone lengths per digit: metacarpal, proximal, intermediate, distal, tip (thumb has no intermediate)
const float kLength[5][5] = {
    {0.05f, 0.05f, 0.035f, 0.025f, 0.0f},   // thumb
    {0.03f, 0.073f, 0.045f, 0.025f, 0.02f},  // index
    {0.01f, 0.091f, 0.049f, 0.03f, 0.02f},   // middle
    {0.02f, 0.073f, 0.045f, 0.03f, 0.03f},   // ring
    {0.03f, 0.067f, 0.03f, 0.025f, 0.02f},   // pinky
};
// rest splay of the metacarpal and proximal joints of index..pinky, degrees
const float kMetacarpalSplay[4] = {13, 0, -15, -27};
const float kProximalSplay[4] = {3, 0, -1, -2};
// fraction of the full curl at which a finger rests on a controller grip
const float kControllerLimit[5] = {0.6f, 0.75f, 0.75f, 0.75f, 0.75f};

const int kParent[kHandBoneCount] = {-1, 0, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19,
                                     1,  21, 22, 23, 24, 0, 0, 0, 0, 0};

// swing: [0] bends the joint (curl), [1] spreads it (splay); twist about the bone axis. Radians.
Quat swing_twist(float curl, float splay, float twist) {
  float sq = curl * curl + splay * splay;
  float ct = std::cos(twist * 0.5f), st = std::sin(twist * 0.5f);
  if (sq <= 0) return Quat{st, 0, 0, ct};
  float theta = std::sqrt(sq), cs = std::cos(theta * 0.5f), ss = std::sin(theta * 0.5f) / theta;
  return Quat{cs * st, splay * ct * ss - curl * st * ss, curl * ct * ss + splay * st * ss, cs * ct};
}

void put(vr::VRBoneTransform_t &t, Quat q, float x, float y, float z) {
  t.position = vr::HmdVector4_t{{x, y, z, 1}};
  t.orientation = vr::HmdQuaternionf_t{q.w, q.x, q.y, q.z};
}

// a bone along its parent's x axis; the right hand is the mirror image, so its offset points the other way
void bone(int hand, Quat q, float length, vr::VRBoneTransform_t &t) { put(t, q, hand ? -length : length, 0, 0); }

// A metacarpal hangs off the wrist, whose axes are turned against the fingers' (the reference skeleton was
// converted between coordinate systems at the wrist only): apply that turn, and rotate the offset with it.
void metacarpal(int hand, Quat q, float length, vr::VRBoneTransform_t &t) {
  Quat o = qmul(Quat{0.5f, -0.5f, 0.5f, 0.5f}, q);
  const float along[3] = {length, 0, 0};
  float p[3];
  qrot(o, along, p);
  if (hand) o = Quat{-o.w, o.z, -o.y, o.x};
  put(t, o, hand ? -p[0] : p[0], p[1], p[2]);
}
}  // namespace

int hand_bone_parent(int bone) { return bone >= 0 && bone < kHandBoneCount ? kParent[bone] : -1; }

const char *hand_bone_name(int hand, int bone) {
  // the names of SteamVR's vr_glove skeletons
  static const char *const names[2][kHandBoneCount] = {
      {"Root", "wrist_l", "finger_thumb_0_l", "finger_thumb_1_l", "finger_thumb_2_l", "finger_thumb_l_end",
       "finger_index_meta_l", "finger_index_0_l", "finger_index_1_l", "finger_index_2_l", "finger_index_l_end",
       "finger_middle_meta_l", "finger_middle_0_l", "finger_middle_1_l", "finger_middle_2_l", "finger_middle_l_end",
       "finger_ring_meta_l", "finger_ring_0_l", "finger_ring_1_l", "finger_ring_2_l", "finger_ring_l_end",
       "finger_pinky_meta_l", "finger_pinky_0_l", "finger_pinky_1_l", "finger_pinky_2_l", "finger_pinky_l_end",
       "finger_thumb_l_aux", "finger_index_l_aux", "finger_middle_l_aux", "finger_ring_l_aux", "finger_pinky_l_aux"},
      {"Root", "wrist_r", "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r", "finger_thumb_r_end",
       "finger_index_meta_r", "finger_index_0_r", "finger_index_1_r", "finger_index_2_r", "finger_index_r_end",
       "finger_middle_meta_r", "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r", "finger_middle_r_end",
       "finger_ring_meta_r", "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r", "finger_ring_r_end",
       "finger_pinky_meta_r", "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r", "finger_pinky_r_end",
       "finger_thumb_r_aux", "finger_index_r_aux", "finger_middle_r_aux", "finger_ring_r_aux", "finger_pinky_r_aux"}};
  return bone >= 0 && bone < kHandBoneCount ? names[hand ? 1 : 0][bone] : nullptr;
}

void hand_pose(int hand, const float curl[5], const float splay[5], vr::VRBoneTransform_t out[kHandBoneCount]) {
  hand = hand ? 1 : 0;
  put(out[kBoneRoot], Quat{}, 0, 0, 0);
  // the wrist relative to the controller's raw pose (measured on an Index controller)
  put(out[kBoneWrist], hand ? Quat{-0.078608f, 0.920279f, -0.379296f, -0.055147f} : Quat{-0.078608f, -0.920279f, 0.379296f, -0.055147f},
      hand ? 0.034038f : -0.034038f, 0.036503f, 0.164722f);

  // thumb: metacarpal, proximal, distal, tip
  metacarpal(hand, swing_twist((10 + curl[0] * 5) * kDeg, (40 + splay[0] * 5) * kDeg, 0), kLength[0][0], out[kBoneThumb0]);
  bone(hand, swing_twist(curl[0] * 90 * kDeg, splay[0] * 20 * kDeg, 0), kLength[0][1], out[kBoneThumb1]);
  bone(hand, swing_twist(curl[0] * 90 * kDeg, 0, 0), kLength[0][2], out[kBoneThumb2]);
  bone(hand, Quat{}, kLength[0][3], out[kBoneThumb3]);

  for (int f = 0; f < 4; f++) {
    float c = curl[f + 1], s = splay[f + 1];
    vr::VRBoneTransform_t *b = &out[kBoneIndex0 + f * 5];
    metacarpal(hand, swing_twist(c * 5 * kDeg, kMetacarpalSplay[f] * kDeg, 0), kLength[f + 1][0], b[0]);
    bone(hand, swing_twist(c * 90 * kDeg, (kProximalSplay[f] + s * 15) * kDeg, 0), kLength[f + 1][1], b[1]);
    bone(hand, swing_twist((5 + c * 80) * kDeg, 0, 0), kLength[f + 1][2], b[2]);
    bone(hand, swing_twist((5 + c * 80) * kDeg, 0, 0), kLength[f + 1][3], b[3]);
    bone(hand, Quat{}, kLength[f + 1][4], b[4]);
  }

  // aux bones: the last knuckle of each digit again, as children of the root
  vr::VRBoneTransform_t model[kHandBoneCount];
  for (int i = kBoneAuxThumb; i < kHandBoneCount; i++) put(out[i], Quat{}, 0, 0, 0);
  hand_to_model_space(out, model);
  out[kBoneAuxThumb] = model[kBoneThumb2];
  for (int f = 0; f < 4; f++) out[kBoneAuxIndex + f] = model[kBoneIndex3 + f * 5];
}

void hand_limit_curls(vr::EVRSkeletalMotionRange range, const float in[5], float out[5]) {
  for (int i = 0; i < 5; i++) {
    float c = in[i] < 0 ? 0 : in[i] > 1 ? 1 : in[i];
    out[i] = range == vr::VRSkeletalMotionRange_WithController ? c * kControllerLimit[i] : c;
  }
}

void hand_reference_pose(int hand, vr::EVRSkeletalReferencePose pose, vr::VRBoneTransform_t out[kHandBoneCount]) {
  const float zero[5] = {0, 0, 0, 0, 0}, one[5] = {1, 1, 1, 1, 1};
  float curl[5];
  switch (pose) {
    case vr::VRSkeletalReferencePose_Fist:
      hand_limit_curls(vr::VRSkeletalMotionRange_WithoutController, one, curl);
      break;
    case vr::VRSkeletalReferencePose_GripLimit:
      hand_limit_curls(vr::VRSkeletalMotionRange_WithController, one, curl);
      break;
    default:  // bind pose and open hand: the flat hand
      hand_limit_curls(vr::VRSkeletalMotionRange_WithoutController, zero, curl);
  }
  hand_pose(hand, curl, zero, out);
}

void hand_to_model_space(const vr::VRBoneTransform_t in[kHandBoneCount], vr::VRBoneTransform_t out[kHandBoneCount]) {
  vr::VRBoneTransform_t m[kHandBoneCount];
  for (int i = 0; i < kHandBoneCount; i++) {
    if (kParent[i] < 0) {
      m[i] = in[i];
      continue;
    }
    const vr::VRBoneTransform_t &p = m[kParent[i]];
    Quat pq{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    Quat lq{in[i].orientation.x, in[i].orientation.y, in[i].orientation.z, in[i].orientation.w};
    float r[3];
    qrot(pq, in[i].position.v, r);
    put(m[i], qnorm(qmul(pq, lq)), p.position.v[0] + r[0], p.position.v[1] + r[1], p.position.v[2] + r[2]);
  }
  for (int i = 0; i < kHandBoneCount; i++) out[i] = m[i];
}
}  // namespace svr
