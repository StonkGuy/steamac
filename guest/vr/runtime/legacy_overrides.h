// SPDX-License-Identifier: Apache-2.0
// Hand-written methods of older interface versions whose signature differs from the current one
// (included by legacy.gen.h; tools/gen_legacy.py emits SVR_LEGACY_BODY_<version>_<method> for each entry).
#pragma once
#include <string.h>
#define SVR_LEGACY_OVERRIDE(version, method)

// IVRInput_004/005: GetPoseActionData became GetPoseActionDataRelativeToNow (same arguments)
SVR_LEGACY_OVERRIDE(IVRInput_004, GetPoseActionData)
SVR_LEGACY_OVERRIDE(IVRInput_005, GetPoseActionData)
#define SVR_LEGACY_BODY_IVRInput_004_GetPoseActionData return c_->GetPoseActionDataRelativeToNow(a0, a1, a2, a3, a4, a5);
#define SVR_LEGACY_BODY_IVRInput_005_GetPoseActionData return c_->GetPoseActionDataRelativeToNow(a0, a1, a2, a3, a4, a5);

// IVRInput_004: GetOriginLocalizedName had no section flags (= all sections)
SVR_LEGACY_OVERRIDE(IVRInput_004, GetOriginLocalizedName)
#define SVR_LEGACY_BODY_IVRInput_004_GetOriginLocalizedName return c_->GetOriginLocalizedName(a0, a1, a2, (int32_t)vr::VRInputString_All);
// IVRInput_004: skeletal calls took a device restriction, the action data carried the bone count (at offset 16 of
// a 24-byte struct), and the transform space was chosen when compressing instead of when decompressing
SVR_LEGACY_OVERRIDE(IVRInput_004, GetSkeletalActionData)
#define SVR_LEGACY_BODY_IVRInput_004_GetSkeletalActionData \
  if (!a1 || a2 < 20) return vr::VRInputError_InvalidParam; \
  vr::EVRInputError e = c_->GetSkeletalActionData(a0, a1, sizeof(vr::InputSkeletalActionData_t)); \
  uint32_t bones = 0; \
  if (e == vr::VRInputError_None) c_->GetBoneCount(a0, &bones); \
  memcpy((char *)a1 + 16, &bones, sizeof(bones)); \
  return e;
SVR_LEGACY_OVERRIDE(IVRInput_004, GetSkeletalBoneData)
#define SVR_LEGACY_BODY_IVRInput_004_GetSkeletalBoneData return c_->GetSkeletalBoneData(a0, a1, a2, a3, a4);
SVR_LEGACY_OVERRIDE(IVRInput_004, GetSkeletalBoneDataCompressed)
#define SVR_LEGACY_BODY_IVRInput_004_GetSkeletalBoneDataCompressed return c_->GetSkeletalBoneDataCompressed(a0, a2, a3, a4, a5);
SVR_LEGACY_OVERRIDE(IVRInput_004, DecompressSkeletalBoneData)
#define SVR_LEGACY_BODY_IVRInput_004_DecompressSkeletalBoneData \
  if (a2) *a2 = vr::VRSkeletalTransformSpace_Parent; \
  return c_->DecompressSkeletalBoneData(a0, a1, vr::VRSkeletalTransformSpace_Parent, a3, a4);
// IVRInput_005: GetSkeletalSummaryData had no summary type (= from the animation)
SVR_LEGACY_OVERRIDE(IVRInput_005, GetSkeletalSummaryData)
#define SVR_LEGACY_BODY_IVRInput_005_GetSkeletalSummaryData return c_->GetSkeletalSummaryData(a0, vr::VRSummaryType_FromAnimation, a1);
// IVRSettings_002: Sync wrote settings to disk; nothing to write
SVR_LEGACY_OVERRIDE(IVRSettings_002, Sync)
#define SVR_LEGACY_BODY_IVRSettings_002_Sync if (a1) *a1 = vr::VRSettingsError_None; return true;
// IVRCompositor_021: explicit timing was on or off
SVR_LEGACY_OVERRIDE(IVRCompositor_021, SetExplicitTimingMode)
#define SVR_LEGACY_BODY_IVRCompositor_021_SetExplicitTimingMode \
  c_->SetExplicitTimingMode(a0 ? vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff : vr::VRCompositorTimingMode_Implicit);
// IVRSystem_017: input focus could be captured; there is no other process to take it from
SVR_LEGACY_OVERRIDE(IVRSystem_017, CaptureInputFocus)
#define SVR_LEGACY_BODY_IVRSystem_017_CaptureInputFocus return true;
// IVROverlay_016-022: ShowKeyboard[ForOverlay] took bUseMinimalMode instead of the flags word
SVR_LEGACY_OVERRIDE(IVROverlay_016, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_016, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_016_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_016_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_017, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_017, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_017_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_017_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_018, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_018, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_018_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_018_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_019, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_019, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_019_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_019_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_020, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_020, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_020_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_020_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_021, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_021, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_021_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_021_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
SVR_LEGACY_OVERRIDE(IVROverlay_022, ShowKeyboard)
SVR_LEGACY_OVERRIDE(IVROverlay_022, ShowKeyboardForOverlay)
#define SVR_LEGACY_BODY_IVROverlay_022_ShowKeyboard return c_->ShowKeyboard(a0, a1, a5 ? vr::KeyboardFlag_Minimal : 0, a2, a3, a4, a6);
#define SVR_LEGACY_BODY_IVROverlay_022_ShowKeyboardForOverlay return c_->ShowKeyboardForOverlay(a0, a1, a2, a6 ? vr::KeyboardFlag_Minimal : 0, a3, a4, a5, a7);
