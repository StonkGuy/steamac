// SPDX-License-Identifier: Apache-2.0
// The SteamVR hand skeleton (31 bones, /skeleton/hand/left and /right) posed from finger curl and splay.
#pragma once
#include "openvr.h"

namespace svr {
enum HandBone {  // the order SteamVR Skeletal Input documents
  kBoneRoot = 0, kBoneWrist,
  kBoneThumb0, kBoneThumb1, kBoneThumb2, kBoneThumb3,
  kBoneIndex0, kBoneIndex1, kBoneIndex2, kBoneIndex3, kBoneIndex4,
  kBoneMiddle0, kBoneMiddle1, kBoneMiddle2, kBoneMiddle3, kBoneMiddle4,
  kBoneRing0, kBoneRing1, kBoneRing2, kBoneRing3, kBoneRing4,
  kBonePinky0, kBonePinky1, kBonePinky2, kBonePinky3, kBonePinky4,
  kBoneAuxThumb, kBoneAuxIndex, kBoneAuxMiddle, kBoneAuxRing, kBoneAuxPinky,
  kHandBoneCount
};

// parent of each bone (-1 for the root); a parent always precedes its children
int hand_bone_parent(int bone);
// hand 0 left, 1 right
const char *hand_bone_name(int hand, int bone);

// Parent-space transforms for curl (0 open .. 1 closed) and splay (-1 .. 1) of thumb, index, middle, ring, pinky.
void hand_pose(int hand, const float curl[5], const float splay[5], vr::VRBoneTransform_t out[kHandBoneCount]);
// The curls a controller in the hand allows: VRSkeletalMotionRange_WithController stops where the fingers
// meet the grip; WithoutController is the full range to a fist.
void hand_limit_curls(vr::EVRSkeletalMotionRange range, const float in[5], float out[5]);
void hand_reference_pose(int hand, vr::EVRSkeletalReferencePose pose, vr::VRBoneTransform_t out[kHandBoneCount]);
// parent space -> model space (relative to the root bone); in and out may be the same array
void hand_to_model_space(const vr::VRBoneTransform_t in[kHandBoneCount], vr::VRBoneTransform_t out[kHandBoneCount]);
}  // namespace svr
