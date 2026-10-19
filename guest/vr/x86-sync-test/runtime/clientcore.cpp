// SPDX-License-Identifier: Apache-2.0
// Entry point of the OpenVR runtime: VRClientCoreFactory → IVRClientCore_003 → GetGenericInterface.
// Proton's vrclient_x64 unix side dlopens <runtime>/bin/linux64/vrclient.so and calls VRClientCoreFactory.
#include <string.h>

#include <cstdlib>
#include <string>

#include "interfaces.h"
#include "log.h"
#include "openvr.h"
#include "runtime.h"

namespace svr {
// IVRClientCore_003 (OpenVR's internal client-core interface; layout from openvr's ivrclientcore.h)
class IVRClientCore {
 public:
  virtual vr::EVRInitError Init(vr::EVRApplicationType type, const char *startup_info) = 0;
  virtual void Cleanup() = 0;
  virtual vr::EVRInitError IsInterfaceVersionValid(const char *version) = 0;
  virtual void *GetGenericInterface(const char *name_and_version, vr::EVRInitError *err) = 0;
  virtual bool BIsHmdPresent() = 0;
  virtual const char *GetEnglishStringForHmdError(vr::EVRInitError err) = 0;
  virtual const char *GetIDForVRInitError(vr::EVRInitError err) = 0;
};

namespace {
struct IfaceEntry {
  const char *version;
  void *(*get)();
};

// One version per interface: the openvr.h this runtime is compiled against (third_party/openvr, v2.15.6).
const IfaceEntry kIfaces[] = {
    {vr::IVRSystem_Version, system_iface},
    {vr::IVRCompositor_Version, compositor_iface},
    {vr::IVRInput_Version, input_iface},
    {vr::IVRChaperone_Version, chaperone_iface},
    {vr::IVRSettings_Version, settings_iface},
    {vr::IVRApplications_Version, applications_iface},
    {vr::IVRRenderModels_Version, render_models_iface},
    {vr::IVROverlay_Version, overlay_iface},
    {vr::IVRScreenshots_Version, screenshots_iface},
    {vr::IVROverlayView_Version, overlay_view_iface},
};

class ClientCore : public IVRClientCore {
 public:
  vr::EVRInitError Init(vr::EVRApplicationType type, const char *startup) override {
    SVR_INFO("Init type %d startup %s", (int)type, startup ? startup : "");
    if (type == vr::VRApplication_Utility || type == vr::VRApplication_Background) {
      // tools may run without a headset; they still get the interfaces
    }
    return Runtime::get().init(type);
  }
  void Cleanup() override { Runtime::get().shutdown(); }
  vr::EVRInitError IsInterfaceVersionValid(const char *v) override {
    return find(v) ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
  }
  void *GetGenericInterface(const char *v, vr::EVRInitError *err) override {
    const IfaceEntry *e = find(v);
    SVR_INFO("requested interface %s%s", v ? v : "(null)", e ? "" : " (not available)");
    if (err) *err = e ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
    return e ? e->get() : nullptr;
  }
  bool BIsHmdPresent() override { return true; }
  const char *GetEnglishStringForHmdError(vr::EVRInitError e) override {
    return e == vr::VRInitError_None ? "No error" : e == vr::VRInitError_Init_HmdNotFound ? "No headset (steamac VR host not connected)"
                                                                                             : "steamac VR error";
  }
  const char *GetIDForVRInitError(vr::EVRInitError e) override {
    switch (e) {
      case vr::VRInitError_None: return "VRInitError_None";
      case vr::VRInitError_Init_HmdNotFound: return "VRInitError_Init_HmdNotFound";
      case vr::VRInitError_Init_InterfaceNotFound: return "VRInitError_Init_InterfaceNotFound";
      default: return "VRInitError_Unknown";
    }
  }

 private:
  static const IfaceEntry *find(const char *v) {
    if (!v) return nullptr;
    if (!strncmp(v, "FnTable:", 8)) v += 8;  // C function tables are built by the caller (Proton) from these
    for (auto &e : kIfaces)
      if (!strcmp(e.version, v)) return &e;
    return nullptr;
  }
};
}  // namespace
}  // namespace svr

extern "C" __attribute__((visibility("default"))) void *VRClientCoreFactory(const char *name, int *ret) {
  static svr::ClientCore core;
  if (name && !strcmp(name, "IVRClientCore_003")) {
    if (ret) *ret = vr::VRInitError_None;
    return static_cast<svr::IVRClientCore *>(&core);
  }
  SVR_ERR("VRClientCoreFactory: unsupported %s", name ? name : "(null)");
  if (ret) *ret = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
