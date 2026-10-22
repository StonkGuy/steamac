// SPDX-License-Identifier: Apache-2.0
// svr-driverhost: runs unmodified SteamVR device drivers (IServerTrackedDeviceProvider, the interface vrserver loads)
// without SteamVR, and publishes the devices they add into the steamac tracked-device table, where the guest OpenVR
// runtime shows them to games (docs/vr-trackers.md). Built twice from this source: a Linux program for Linux drivers
// (bin/linux64/driver_<name>.so) and a Windows program run under Wine/Proton for Windows drivers
// (bin/win64/driver_<name>.dll), which maps the same table through Wine's Z: drive.
//
//   svr-driverhost [--table PATH] [--hz N] [--seconds N] [--verbose] [DRIVER_DIR...] [--then PROGRAM [ARGS...]]
//
// --then starts PROGRAM (a tracker product's companion application) once the drivers are loaded and exits when it
// exits. Run that way, the driver and its application share one process tree (and under Wine one wineserver, which
// their pipes and shared memory need). Without a DRIVER_DIR, PROGRAM's directory is used when it holds a
// driver.vrdrivermanifest (products that install their driver beside the application, such as Standable).
//
// DRIVER_DIR is a driver's root (the directory with driver.vrdrivermanifest). Only what a tracker driver needs from
// vrserver is provided: device registration and properties, poses, settings (the driver's default.vrsettings plus
// changes in memory), resources, logging, and input components that are accepted and ignored. HMD and hand
// controller devices are refused (the headset link provides those); drivers that need a compositor, display or
// camera component are out of scope.
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "openvr_driver.h"
#include "trackers.h"

namespace dh {
using namespace vr;

std::atomic<bool> g_exit{false};
bool g_verbose = false;

void logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "svr-driverhost: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

int64_t now_ns() {
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

void sleep_ms(int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)ms * 1000);
#endif
}

uint32_t copy_out(const std::string &s, char *buf, uint32_t len) {
  uint32_t need = (uint32_t)s.size() + 1;
  if (buf && len) {
    uint32_t n = need <= len ? need - 1 : len - 1;
    memcpy(buf, s.data(), n);
    buf[n] = 0;
  }
  return need;
}

// --- quaternion helpers (driver pose -> stage pose) ---
struct Q {
  double w, x, y, z;
};
Q qmul(Q a, Q b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
void qrot(Q q, const double v[3], double out[3]) {
  Q p{0, v[0], v[1], v[2]};
  Q r = qmul(qmul(q, p), Q{q.w, -q.x, -q.y, -q.z});
  out[0] = r.x, out[1] = r.y, out[2] = r.z;
}
Q from(const HmdQuaternion_t &q) {
  // an all-zero quaternion (common in drivers that leave the transforms unset) means identity
  if (q.w == 0 && q.x == 0 && q.y == 0 && q.z == 0) return {1, 0, 0, 0};
  return {q.w, q.x, q.y, q.z};
}

// DriverPose_t -> stage-space svr_pose, as vrserver composes it:
//   position    = worldFromDriver * (vecPosition + qRotation * driverFromHead.translation)
//   orientation = qWorldFromDriver * qRotation * qDriverFromHead
svr_pose to_stage(const DriverPose_t &p) {
  svr_pose o{};
  Q wd = from(p.qWorldFromDriverRotation), r = from(p.qRotation), dh = from(p.qDriverFromHeadRotation);
  double off[3], local[3], world[3];
  qrot(r, p.vecDriverFromHeadTranslation, off);
  for (int i = 0; i < 3; i++) local[i] = p.vecPosition[i] + off[i];
  qrot(wd, local, world);
  Q q = qmul(qmul(wd, r), dh);
  double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n > 0) q = {q.w / n, q.x / n, q.y / n, q.z / n};
  o.orientation[0] = (float)q.x, o.orientation[1] = (float)q.y, o.orientation[2] = (float)q.z, o.orientation[3] = (float)q.w;
  double v[3], w[3];
  qrot(wd, p.vecVelocity, v);
  qrot(wd, p.vecAngularVelocity, w);  // driver angular velocity is in driver world space
  for (int i = 0; i < 3; i++) {
    o.position[i] = (float)(world[i] + p.vecWorldFromDriverTranslation[i]);
    o.lin_vel[i] = (float)v[i];
    o.ang_vel[i] = (float)w[i];
  }
  bool valid = p.poseIsValid && p.deviceIsConnected;
  o.flags = (valid ? SVR_POSE_VALID | SVR_POSE_VEL : 0) |
            (valid && p.result == TrackingResult_Running_OK ? SVR_POSE_TRACKED : 0);
  return o;
}

// --- properties ---
struct Prop {
  PropertyTypeTag_t tag = k_unInvalidPropertyTag;
  std::vector<uint8_t> data;
  ETrackedPropertyError set_error = TrackedProp_Success;
};
using PropMap = std::map<int, Prop>;

struct Device {
  std::string serial;
  ETrackedDeviceClass cls = TrackedDeviceClass_Invalid;
  ITrackedDeviceServerDriver *driver = nullptr;
  uint32_t object_id = 0;  // = OpenVR device index in the runtime (kTrackerBase + table slot)
  int slot = -1;
  bool activating = false, activated = false, published = false;
  PropMap props;
  svr_pose pose{};
  bool has_pose = false;
};

struct Driver;

class Host;
Host *g_host = nullptr;

class Host : public IVRDriverContext,
             public IVRServerDriverHost,
             public IVRProperties,
             public IVRSettings,
             public IVRDriverLog,
             public IVRDriverInput,
             public IVRResources,
             public IVRDriverManager,
             public IVRIOBuffer,
             public IVRDriverSpatialAnchors {
 public:
  svr::TrackerTable table;
  svr::TrackerView view;
  std::mutex m;
  std::vector<std::unique_ptr<Device>> devices;
  std::vector<std::string> driver_names, driver_dirs;
  std::string current_driver;  // the driver being initialised / logging
  std::map<std::string, svr::Json> settings;  // section -> object (defaults from the drivers' default.vrsettings)
  std::map<std::string, std::string> set_values;  // "section\nkey" -> JSON-ish scalar set at run time
  uint64_t next_component = 1;

  // ---------- IVRDriverContext ----------
  void *GetGenericInterface(const char *v, EVRInitError *err) override {
    void *p = nullptr;
    std::string s = v ? v : "";
    auto is = [&](const char *prefix) { return s.compare(0, strlen(prefix), prefix) == 0; };
    // older interface versions this host serves with the current layout (methods only appended since)
    if (is("IVRServerDriverHost_00")) p = static_cast<IVRServerDriverHost *>(this);
    else if (s == IVRProperties_Version) p = static_cast<IVRProperties *>(this);
    else if (s == IVRSettings_Version || s == "IVRSettings_002") p = static_cast<IVRSettings *>(this);
    else if (s == IVRDriverLog_Version) p = static_cast<IVRDriverLog *>(this);
    else if (s == IVRDriverInput_Version || s == "IVRDriverInput_003") p = static_cast<IVRDriverInput *>(this);
    else if (s == IVRResources_Version) p = static_cast<IVRResources *>(this);
    else if (s == IVRDriverManager_Version) p = static_cast<IVRDriverManager *>(this);
    else if (s == IVRIOBuffer_Version) p = static_cast<IVRIOBuffer *>(this);
    else if (s == IVRDriverSpatialAnchors_Version) p = static_cast<IVRDriverSpatialAnchors *>(this);
    if (!p) logf("%s asked for %s: not provided", current_driver.c_str(), s.c_str());
    else if (g_verbose) logf("%s: interface %s", current_driver.c_str(), s.c_str());
    if (err) *err = p ? VRInitError_None : VRInitError_Init_InterfaceNotFound;
    return p;
  }
  DriverHandle_t GetDriverHandle() override { return kDriverContainer; }

  // ---------- IVRServerDriverHost ----------
  bool TrackedDeviceAdded(const char *serial, ETrackedDeviceClass cls, ITrackedDeviceServerDriver *drv) override {
    std::lock_guard<std::mutex> g(m);
    if (!serial || !*serial || !drv) return false;
    if (cls == TrackedDeviceClass_HMD || cls == TrackedDeviceClass_DisplayRedirect) {
      logf("%s: device %s (class %d) refused: the headset comes from the steamac VR link", current_driver.c_str(),
           serial, (int)cls);
      return false;
    }
    for (auto &d : devices)
      if (d->serial == serial) return false;  // SteamVR refuses a duplicate serial as well
    auto d = std::make_unique<Device>();
    d->serial = serial;
    d->cls = cls;
    d->driver = drv;
    svr_trk_device desc{};
    snprintf(desc.serial, sizeof desc.serial, "%s", serial);
    desc.device_class = (uint32_t)cls;
    desc.writer_pid = (uint32_t)getpid_();
    d->slot = table.claim(desc, false);  // reserved: games see the device once it is activated and has a pose
    if (d->slot < 0) {
      logf("%s: device %s refused: tracked-device table full", current_driver.c_str(), serial);
      return false;
    }
    d->object_id = 3 + (uint32_t)d->slot;  // svr::kTrackerBase
    logf("%s: device %s class %d -> device index %u", current_driver.c_str(), serial, (int)cls, d->object_id);
    devices.push_back(std::move(d));
    return true;  // Activate follows from the main loop, outside the driver's call (as in vrserver)
  }

  void TrackedDevicePoseUpdated(uint32_t id, const DriverPose_t &pose, uint32_t size) override {
    if (size < sizeof(DriverPose_t) - sizeof(bool) * 2) return;
    std::lock_guard<std::mutex> g(m);
    Device *d = by_id(id);
    if (!d) return;
    d->pose = to_stage(pose);
    bool first = !d->has_pose;
    d->has_pose = true;
    if (d->published) table.update(d->slot, d->pose, now_ns());
    else if (first && d->activated) publish(d);
  }

  void VsyncEvent(double) override {}
  void VendorSpecificEvent(uint32_t, EVREventType, const VREvent_Data_t &, double) override {}
  bool IsExiting() override { return g_exit.load(); }
  bool PollNextEvent(VREvent_t *, uint32_t) override { return false; }

  // poses of every device as the games see them: headset and hands from the scene app's mirror, table devices
  void GetRawTrackedDevicePoses(float predict, TrackedDevicePose_t *out, uint32_t count) override {
    if (!out) return;
    std::lock_guard<std::mutex> g(m);
    int64_t now = now_ns();
    svr_trk_mirror mir{};
    bool live = view.mirror_live(table, now, &mir);
    view.refresh(table, now);
    for (uint32_t i = 0; i < count; i++) {
      svr_pose p{};
      bool conn = false;
      if (i < 3) {
        static const int map[3] = {SVR_DEV_HEAD, SVR_DEV_LEFT_GRIP, SVR_DEV_RIGHT_GRIP};
        p = mir.pose[map[i]];
        conn = live && (i == 0 || (p.flags & SVR_POSE_VALID));
      } else if (i < 3 + SVR_TRK_MAX) {
        const svr::TrackerView::Dev &v = view.dev[i - 3];
        p = v.d.pose;
        conn = v.connected;
      }
      out[i] = to_openvr(p, conn, predict);
    }
  }
  void RequestRestart(const char *reason, const char *, const char *, const char *) override {
    logf("%s requests a restart: %s", current_driver.c_str(), reason ? reason : "");
  }
  uint32_t GetFrameTimings(Compositor_FrameTiming *, uint32_t) override { return 0; }
  void SetDisplayEyeToHead(uint32_t, const HmdMatrix34_t &, const HmdMatrix34_t &) override {}
  void SetDisplayProjectionRaw(uint32_t, const HmdRect2_t &, const HmdRect2_t &) override {}
  void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}

  // ---------- IVRProperties ----------
  ETrackedPropertyError ReadPropertyBatch(PropertyContainerHandle_t c, PropertyRead_t *batch, uint32_t n) override {
    std::lock_guard<std::mutex> g(m);
    PropMap *pm = container(c);
    if (!pm) return TrackedProp_InvalidContainer;
    for (uint32_t i = 0; i < n; i++) {
      PropertyRead_t &r = batch[i];
      auto it = pm->find((int)r.prop);
      if (it == pm->end()) {
        r.unTag = k_unInvalidPropertyTag, r.unRequiredBufferSize = 0, r.eError = TrackedProp_UnknownProperty;
        continue;
      }
      const Prop &p = it->second;
      if (p.set_error != TrackedProp_Success) {
        r.eError = p.set_error;
        continue;
      }
      r.unTag = p.tag;
      r.unRequiredBufferSize = (uint32_t)p.data.size();
      if (!r.pvBuffer || r.unBufferSize < p.data.size()) {
        r.eError = TrackedProp_BufferTooSmall;
        continue;
      }
      memcpy(r.pvBuffer, p.data.data(), p.data.size());
      r.eError = TrackedProp_Success;
    }
    return TrackedProp_Success;
  }
  ETrackedPropertyError WritePropertyBatch(PropertyContainerHandle_t c, PropertyWrite_t *batch, uint32_t n) override {
    std::lock_guard<std::mutex> g(m);
    PropMap *pm = container(c);
    if (!pm) return TrackedProp_InvalidContainer;
    for (uint32_t i = 0; i < n; i++) {
      PropertyWrite_t &w = batch[i];
      if (w.writeType == PropertyWrite_Erase) pm->erase((int)w.prop);
      else if (w.writeType == PropertyWrite_SetError) (*pm)[(int)w.prop].set_error = w.eSetError;
      else {
        Prop &p = (*pm)[(int)w.prop];
        p.tag = w.unTag;
        p.set_error = TrackedProp_Success;
        p.data.assign((const uint8_t *)w.pvBuffer, (const uint8_t *)w.pvBuffer + w.unBufferSize);
      }
      w.eError = TrackedProp_Success;
    }
    if (Device *d = by_container(c)) {
      if (d->published) publish(d);  // a later change of a string property reaches the table too
    }
    return TrackedProp_Success;
  }
  const char *GetPropErrorNameFromEnum(ETrackedPropertyError e) override {
    return e == TrackedProp_Success ? "TrackedProp_Success" : "TrackedProp_Error";
  }
  PropertyContainerHandle_t TrackedDeviceToPropertyContainer(TrackedDeviceIndex_t i) override {
    std::lock_guard<std::mutex> g(m);
    return by_id(i) ? (PropertyContainerHandle_t)i + kDeviceContainerBase : k_ulInvalidPropertyContainer;
  }

  // ---------- IVRSettings ----------
  const char *GetSettingsErrorNameFromEnum(EVRSettingsError e) override {
    return e == VRSettingsError_None ? "VRSettingsError_None" : "VRSettingsError_UnsetSettingHasNoDefault";
  }
  void SetBool(const char *s, const char *k, bool v, EVRSettingsError *e) override { set(s, k, v ? "true" : "false", e); }
  void SetInt32(const char *s, const char *k, int32_t v, EVRSettingsError *e) override { set(s, k, std::to_string(v), e); }
  void SetFloat(const char *s, const char *k, float v, EVRSettingsError *e) override { set(s, k, std::to_string(v), e); }
  void SetString(const char *s, const char *k, const char *v, EVRSettingsError *e) override {
    set(s, k, std::string("\"") + (v ? v : ""), e);
  }
  bool GetBool(const char *s, const char *k, EVRSettingsError *e) override {
    std::string v;
    if (!get(s, k, &v, e)) return false;
    return v == "true" || v == "1";
  }
  int32_t GetInt32(const char *s, const char *k, EVRSettingsError *e) override {
    std::string v;
    return get(s, k, &v, e) ? (int32_t)atof(v.c_str()) : 0;
  }
  float GetFloat(const char *s, const char *k, EVRSettingsError *e) override {
    std::string v;
    return get(s, k, &v, e) ? (float)atof(v.c_str()) : 0.0f;
  }
  void GetString(const char *s, const char *k, char *buf, uint32_t len, EVRSettingsError *e) override {
    std::string v;
    if (!get(s, k, &v, e)) v.clear();
    else if (!v.empty() && v[0] == '"') v.erase(0, 1);
    copy_out(v, buf, len);
  }
  void RemoveSection(const char *s, EVRSettingsError *e) override {
    std::lock_guard<std::mutex> g(m);
    std::string pre = std::string(s ? s : "") + "\n";
    for (auto it = set_values.begin(); it != set_values.end();)
      it = it->first.compare(0, pre.size(), pre) ? std::next(it) : set_values.erase(it);
    if (e) *e = VRSettingsError_None;
  }
  void RemoveKeyInSection(const char *s, const char *k, EVRSettingsError *e) override {
    std::lock_guard<std::mutex> g(m);
    set_values.erase(std::string(s ? s : "") + "\n" + (k ? k : ""));
    if (e) *e = VRSettingsError_None;
  }

  // ---------- IVRDriverLog ----------
  void Log(const char *msg) override {
    if (!msg) return;
    std::string s = msg;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    fprintf(stderr, "svr-driverhost: [%s] %s\n", current_driver.c_str(), s.c_str());
  }

  // ---------- IVRDriverInput (accepted, ignored: trackers carry no input a game reads) ----------
  EVRInputError CreateBooleanComponent(PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *h) override {
    return component(h);
  }
  EVRInputError UpdateBooleanComponent(VRInputComponentHandle_t, bool, double) override { return VRInputError_None; }
  EVRInputError CreateScalarComponent(PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *h, EVRScalarType,
                                      EVRScalarUnits) override {
    return component(h);
  }
  EVRInputError UpdateScalarComponent(VRInputComponentHandle_t, float, double) override { return VRInputError_None; }
  EVRInputError CreateHapticComponent(PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *h) override {
    return component(h);
  }
  EVRInputError CreateSkeletonComponent(PropertyContainerHandle_t, const char *, const char *, const char *,
                                        EVRSkeletalTrackingLevel, const VRBoneTransform_t *, uint32_t,
                                        VRInputComponentHandle_t *h) override {
    return component(h);
  }
  EVRInputError UpdateSkeletonComponent(VRInputComponentHandle_t, EVRSkeletalMotionRange, const VRBoneTransform_t *,
                                        uint32_t) override {
    return VRInputError_None;
  }
  EVRInputError CreatePoseComponent(PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *h) override {
    return component(h);
  }
  EVRInputError UpdatePoseComponent(VRInputComponentHandle_t, const HmdMatrix34_t *, double) override {
    return VRInputError_None;
  }
  EVRInputError CreateEyeTrackingComponent(PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *h) override {
    return component(h);
  }
  EVRInputError UpdateEyeTrackingComponent(VRInputComponentHandle_t, const VREyeTrackingData_t *, double) override {
    return VRInputError_None;
  }

  // ---------- IVRResources ----------
  uint32_t LoadSharedResource(const char *name, char *buf, uint32_t len) override {
    std::string path = resolve(name ? name : "", "");
    std::string data;
    if (path.empty() || !svr::read_file(path, &data)) return 0;
    if (buf && len >= data.size()) memcpy(buf, data.data(), data.size());
    return (uint32_t)data.size();
  }
  uint32_t GetResourceFullPath(const char *name, const char *dir, char *buf, uint32_t len) override {
    std::string path = resolve(name ? name : "", dir ? dir : "");
    if (path.empty()) {
      if (buf && len) buf[0] = 0;
      return 0;
    }
    return copy_out(path, buf, len);
  }

  // ---------- IVRDriverManager ----------
  uint32_t GetDriverCount() const override { return (uint32_t)driver_names.size(); }
  uint32_t GetDriverName(DriverId_t i, char *buf, uint32_t len) override {
    return i < driver_names.size() ? copy_out(driver_names[i], buf, len) : 0;
  }
  DriverHandle_t GetDriverHandle(const char *name) override {
    for (size_t i = 0; i < driver_names.size(); i++)
      if (name && driver_names[i] == name) return kDriverContainer;
    return k_ulInvalidPropertyContainer;
  }
  bool IsEnabled(DriverId_t i) const override { return i < driver_names.size(); }

  // ---------- IVRIOBuffer / IVRDriverSpatialAnchors (not available) ----------
  EIOBufferError Open(const char *, EIOBufferMode, uint32_t, uint32_t, IOBufferHandle_t *) override {
    return IOBuffer_OperationFailed;
  }
  EIOBufferError Close(IOBufferHandle_t) override { return IOBuffer_OperationFailed; }
  EIOBufferError Read(IOBufferHandle_t, void *, uint32_t, uint32_t *) override { return IOBuffer_OperationFailed; }
  EIOBufferError Write(IOBufferHandle_t, void *, uint32_t) override { return IOBuffer_OperationFailed; }
  PropertyContainerHandle_t PropertyContainer(IOBufferHandle_t) override { return k_ulInvalidPropertyContainer; }
  bool HasReaders(IOBufferHandle_t) override { return false; }
  EVRSpatialAnchorError UpdateSpatialAnchorPose(SpatialAnchorHandle_t, const SpatialAnchorDriverPose_t *) override {
    return VRSpatialAnchorError_Internal;
  }
  EVRSpatialAnchorError SetSpatialAnchorPoseError(SpatialAnchorHandle_t, EVRSpatialAnchorError, double) override {
    return VRSpatialAnchorError_Internal;
  }
  EVRSpatialAnchorError UpdateSpatialAnchorDescriptor(SpatialAnchorHandle_t, const char *) override {
    return VRSpatialAnchorError_Internal;
  }
  EVRSpatialAnchorError GetSpatialAnchorPose(SpatialAnchorHandle_t, SpatialAnchorDriverPose_t *) override {
    return VRSpatialAnchorError_Internal;
  }
  EVRSpatialAnchorError GetSpatialAnchorDescriptor(SpatialAnchorHandle_t, char *, uint32_t *, bool) override {
    return VRSpatialAnchorError_Internal;
  }

  // ---------- host side ----------
  static constexpr PropertyContainerHandle_t kDriverContainer = 0x100000000ull;
  static constexpr PropertyContainerHandle_t kDeviceContainerBase = 0x200000000ull;
  PropMap driver_props;

  static long getpid_() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
  }

  Device *by_id(uint32_t id) {
    for (auto &d : devices)
      if (d->object_id == id) return d.get();
    return nullptr;
  }
  Device *by_container(PropertyContainerHandle_t c) {
    return c >= kDeviceContainerBase && c < kDeviceContainerBase + 64 ? by_id((uint32_t)(c - kDeviceContainerBase))
                                                                       : nullptr;
  }
  PropMap *container(PropertyContainerHandle_t c) {
    if (c == kDriverContainer) return &driver_props;
    Device *d = by_container(c);
    return d ? &d->props : nullptr;
  }
  std::string str_prop(Device *d, ETrackedDeviceProperty p) {
    auto it = d->props.find((int)p);
    if (it == d->props.end() || it->second.tag != k_unStringPropertyTag || it->second.data.empty()) return "";
    return std::string((const char *)it->second.data.data(), strnlen((const char *)it->second.data.data(), it->second.data.size()));
  }

  // the device's string properties and role into its table slot (the role from steamvr.vrsettings-style
  // "trackers" settings "/devices/<driver>/<serial>", else from a vive_tracker_<role> controller type)
  void publish(Device *d) {
    svr_trk_device desc{};
    auto put = [&](char *dst, size_t cap, ETrackedDeviceProperty p) { snprintf(dst, cap, "%s", str_prop(d, p).c_str()); };
    snprintf(desc.serial, sizeof desc.serial, "%s", d->serial.c_str());
    desc.device_class = (uint32_t)d->cls;
    put(desc.model, sizeof desc.model, Prop_ModelNumber_String);
    put(desc.manufacturer, sizeof desc.manufacturer, Prop_ManufacturerName_String);
    put(desc.tracking_system, sizeof desc.tracking_system, Prop_TrackingSystemName_String);
    put(desc.controller_type, sizeof desc.controller_type, Prop_ControllerType_String);
    put(desc.render_model, sizeof desc.render_model, Prop_RenderModelName_String);
    put(desc.input_profile, sizeof desc.input_profile, Prop_InputProfilePath_String);
    std::string role;
    std::string sys = desc.tracking_system[0] ? desc.tracking_system : current_driver;
    get_locked("trackers", ("/devices/" + sys + "/" + d->serial).c_str(), &role);
    if (!role.empty() && role[0] == '"') role.erase(0, 1);
    desc.role = svr::tracker_role_from_name(role.c_str());
    if (!desc.role) desc.role = svr::tracker_role_from_name(desc.controller_type);
    desc.writer_pid = (uint32_t)getpid_();
    desc.updated = now_ns();
    int slot = table.claim(desc);  // same serial: the same slot, strings replaced
    if (slot != d->slot) logf("device %s moved from slot %d to %d", d->serial.c_str(), d->slot, slot);
    if (!d->published)
      logf("device %s published: index %u type %s model %s role %s", d->serial.c_str(), d->object_id,
           desc.controller_type[0] ? desc.controller_type : "(default)", desc.model[0] ? desc.model : "(default)",
           svr::tracker_role_setting(desc.role));
    d->published = true;
    if (d->has_pose) table.update(d->slot, d->pose, now_ns());
  }

  // activates added devices; refreshes every published device's pose so it stays connected while the driver runs
  // (SteamVR keeps a device's last pose until the driver changes it)
  void service() {
    std::vector<Device *> todo;
    {
      std::lock_guard<std::mutex> g(m);
      for (auto &d : devices)
        if (!d->activating) d->activating = true, todo.push_back(d.get());
    }
    for (Device *d : todo) {
      EVRInitError e = d->driver->Activate(d->object_id);  // outside the lock: the driver writes properties here
      std::lock_guard<std::mutex> g(m);
      if (e != VRInitError_None) {
        logf("device %s: Activate failed (%d)", d->serial.c_str(), (int)e);
        table.release(d->slot);
        continue;
      }
      d->activated = true;
      if (d->has_pose) publish(d);  // else on its first pose update
    }
    std::lock_guard<std::mutex> g(m);
    int64_t now = now_ns();
    for (auto &d : devices)
      if (d->published && d->has_pose) table.update(d->slot, d->pose, now);
  }

  void shutdown() {
    std::lock_guard<std::mutex> g(m);
    for (auto &d : devices)
      if (d->slot >= 0) table.release(d->slot);
  }

  static TrackedDevicePose_t to_openvr(const svr_pose &p, bool conn, float predict) {
    TrackedDevicePose_t t{};
    t.bDeviceIsConnected = conn;
    t.bPoseIsValid = conn && (p.flags & SVR_POSE_VALID);
    t.eTrackingResult = conn ? TrackingResult_Running_OK : TrackingResult_Uninitialized;
    double x = p.orientation[0], y = p.orientation[1], z = p.orientation[2], w = p.orientation[3];
    if (!t.bPoseIsValid) x = y = z = 0, w = 1;
    double pos[3];
    for (int i = 0; i < 3; i++) pos[i] = t.bPoseIsValid ? p.position[i] + p.lin_vel[i] * predict : 0;
    HmdMatrix34_t &m = t.mDeviceToAbsoluteTracking;
    m.m[0][0] = (float)(1 - 2 * (y * y + z * z)), m.m[0][1] = (float)(2 * (x * y - z * w)), m.m[0][2] = (float)(2 * (x * z + y * w));
    m.m[1][0] = (float)(2 * (x * y + z * w)), m.m[1][1] = (float)(1 - 2 * (x * x + z * z)), m.m[1][2] = (float)(2 * (y * z - x * w));
    m.m[2][0] = (float)(2 * (x * z - y * w)), m.m[2][1] = (float)(2 * (y * z + x * w)), m.m[2][2] = (float)(1 - 2 * (x * x + y * y));
    for (int i = 0; i < 3; i++) {
      m.m[i][3] = (float)pos[i];
      t.vVelocity.v[i] = p.lin_vel[i];
      t.vAngularVelocity.v[i] = p.ang_vel[i];
    }
    return t;
  }

  EVRInputError component(VRInputComponentHandle_t *h) {
    std::lock_guard<std::mutex> g(m);
    if (h) *h = next_component++;
    return VRInputError_None;
  }

  // "{driver_name}/resources/x" or a path relative to the driver's resources/<dir>
  std::string resolve(const std::string &name, const std::string &dir) {
    std::string rel = name;
    std::string root = driver_dirs.empty() ? "" : driver_dirs.back();
    if (!rel.empty() && rel[0] == '{') {
      size_t e = rel.find('}');
      if (e == std::string::npos) return "";
      std::string drv = rel.substr(1, e - 1);
      for (size_t i = 0; i < driver_names.size(); i++)
        if (driver_names[i] == drv) root = driver_dirs[i];
      rel = rel.substr(e + 1);
      if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
      return root + "/" + rel;
    }
    if (!rel.empty() && (rel[0] == '/' || (rel.size() > 2 && rel[1] == ':'))) return rel;
    return root + "/resources/" + (dir.empty() ? "" : dir + "/") + rel;
  }

  void set(const char *s, const char *k, const std::string &v, EVRSettingsError *e) {
    std::lock_guard<std::mutex> g(m);
    set_values[std::string(s ? s : "") + "\n" + (k ? k : "")] = v;
    if (e) *e = VRSettingsError_None;
  }
  bool get(const char *s, const char *k, std::string *out, EVRSettingsError *e) {
    std::lock_guard<std::mutex> g(m);
    bool ok = get_locked(s, k, out);
    if (e) *e = ok ? VRSettingsError_None : VRSettingsError_UnsetSettingHasNoDefault;
    return ok;
  }
  bool get_locked(const char *s, const char *k, std::string *out) {
    std::string sec = s ? s : "", key = k ? k : "";
    auto sv = set_values.find(sec + "\n" + key);
    if (sv != set_values.end()) {
      *out = sv->second;
      return true;
    }
    auto it = settings.find(sec);
    if (it == settings.end()) return false;
    const svr::Json *v = it->second.get(key);
    if (!v) return false;
    switch (v->type) {
      case svr::Json::Bool: *out = v->b ? "true" : "false"; return true;
      case svr::Json::Number: *out = std::to_string(v->num); return true;
      case svr::Json::String: *out = "\"" + v->str; return true;
      default: return false;
    }
  }
  void merge_settings(const svr::Json &root) {
    if (!root.is_obj()) return;
    for (auto &sec : root.obj) {
      if (!sec.second.is_obj()) continue;
      svr::Json &dst = settings[sec.first];
      dst.type = svr::Json::Object;
      for (auto &kv : sec.second.obj) {
        bool replaced = false;
        for (auto &old : dst.obj)
          if (old.first == kv.first) old.second = kv.second, replaced = true;
        if (!replaced) dst.obj.push_back(kv);
      }
    }
  }
};

typedef void *(*Factory)(const char *, int *);

struct Loaded {
  std::string name, dir;
  IServerTrackedDeviceProvider *provider = nullptr;
};

// load one driver directory: manifest name, library, default settings, provider Init
bool load_driver(Host &h, const std::string &dir, Loaded *out) {
  svr::Json manifest;
  std::string text, err, name;
  if (svr::read_file(dir + "/driver.vrdrivermanifest", &text) && svr::Json::parse(text, &manifest, &err))
    name = manifest.get_str("name");
  if (name.empty()) {
    logf("%s: no driver.vrdrivermanifest with a \"name\"", dir.c_str());
    return false;
  }
#ifdef _WIN32
  std::string bin = dir + "/bin/win64";
  std::string lib = bin + "/driver_" + name + ".dll";
  SetDllDirectoryA(bin.c_str());  // the driver's own DLLs beside it
  HMODULE mod = LoadLibraryA(lib.c_str());
  Factory f = mod ? (Factory)(void *)GetProcAddress(mod, "HmdDriverFactory") : nullptr;
  if (!mod) logf("%s: LoadLibrary failed (%lu)", lib.c_str(), (unsigned long)GetLastError());
#else
  std::string lib = dir + "/bin/linux64/driver_" + name + ".so";
  void *mod = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  Factory f = mod ? (Factory)dlsym(mod, "HmdDriverFactory") : nullptr;
  if (!mod) logf("%s: %s", lib.c_str(), dlerror());
#endif
  if (!f) {
    if (mod) logf("%s: no HmdDriverFactory", lib.c_str());
    return false;
  }
  svr::Json defaults;
  if (svr::read_file(dir + "/resources/settings/default.vrsettings", &text) && svr::Json::parse(text, &defaults, &err))
    h.merge_settings(defaults);
  h.driver_names.push_back(name);
  h.driver_dirs.push_back(dir);
  h.current_driver = name;
  int rc = 0;
  auto *prov = (IServerTrackedDeviceProvider *)f(IServerTrackedDeviceProvider_Version, &rc);
  if (!prov) {
    logf("%s: HmdDriverFactory(%s) failed (%d)", name.c_str(), IServerTrackedDeviceProvider_Version, rc);
    return false;
  }
  EVRInitError e = prov->Init(static_cast<IVRDriverContext *>(&h));
  if (e != VRInitError_None) {
    logf("%s: Init failed (%d)", name.c_str(), (int)e);
    return false;
  }
  logf("%s: loaded from %s", name.c_str(), lib.c_str());
  out->name = name, out->dir = dir, out->provider = prov;
  return true;
}

// the companion application started with --then
struct Child {
#ifdef _WIN32
  HANDLE process = nullptr;
#else
  pid_t pid = 0;
#endif
  bool start(const std::vector<std::string> &argv) {
    if (argv.empty()) return false;
#ifdef _WIN32
    std::string cmd;
    for (auto &a : argv) cmd += (cmd.empty() ? "" : " ") + ("\"" + a + "\"");
    std::string dir = argv[0].substr(0, argv[0].find_last_of("\\/"));
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(),
                        &si, &pi)) {
      logf("cannot start %s (%lu)", argv[0].c_str(), (unsigned long)GetLastError());
      return false;
    }
    CloseHandle(pi.hThread);
    process = pi.hProcess;
#else
    pid = fork();
    if (pid == 0) {
      std::vector<char *> a;
      for (auto &s : argv) a.push_back(const_cast<char *>(s.c_str()));
      a.push_back(nullptr);
      execv(a[0], a.data());
      _exit(127);
    }
    if (pid < 0) return false;
#endif
    logf("started %s", argv[0].c_str());
    return true;
  }
  bool running() {
#ifdef _WIN32
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
    return pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0;
#endif
  }
};

std::string dir_of(const std::string &path) {
  size_t e = path.find_last_of("\\/");
  return e == std::string::npos ? "." : path.substr(0, e);
}

#ifdef _WIN32
BOOL WINAPI on_ctrl(DWORD) {
  g_exit = true;
  return TRUE;
}
#else
void on_signal(int) { g_exit = true; }
#endif

int run(int argc, char **argv) {
  std::string table_path;
  double hz = 90, seconds = 0;
  std::vector<std::string> dirs, then;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--then") {
      for (i++; i < argc; i++) then.push_back(argv[i]);
      break;
    }
    if (a == "--table" && i + 1 < argc) table_path = argv[++i];
    else if (a == "--hz" && i + 1 < argc) hz = atof(argv[++i]);
    else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if (a == "--verbose") g_verbose = true;
    else if (a == "-h" || a == "--help") {
      fprintf(stderr, "usage: svr-driverhost [--table PATH] [--hz N] [--seconds N] [--verbose] [DRIVER_DIR...] [--then PROGRAM ARGS...]\n");
      return 0;
    } else dirs.push_back(a);
  }
#ifdef _WIN32
  // Steam hands Proton a Unix path for the application; Wine sees the same file through Z:
  if (!then.empty() && !then[0].empty() && then[0][0] == '/') {
    std::string w = "Z:" + then[0];
    for (char &c : w)
      if (c == '/') c = '\\';
    then[0] = w;
  }
#endif
  if (dirs.empty() && !then.empty()) {
    std::string text, d = dir_of(then[0]);
    if (svr::read_file(d + "/driver.vrdrivermanifest", &text)) dirs.push_back(d);
  }
  if (dirs.empty()) {
    fprintf(stderr, "usage: svr-driverhost [--table PATH] [--hz N] [--seconds N] [--verbose] [DRIVER_DIR...] [--then PROGRAM ARGS...]\n");
    return 2;
  }
  static Host h;
  g_host = &h;
  if (!h.table.open(table_path.empty() ? nullptr : table_path.c_str(), true)) {
    logf("cannot open the tracked-device table %s", table_path.empty() ? svr::TrackerTable::default_path().c_str()
                                                                        : table_path.c_str());
    return 1;
  }
#ifdef _WIN32
  SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
#endif
  std::vector<Loaded> drivers;
  for (auto &d : dirs) {
    Loaded l;
    if (load_driver(h, d, &l)) drivers.push_back(l);
  }
  Child child;
  if (drivers.empty()) {
    // the application still starts (it is the product the user asked for); it only lacks its trackers
    if (!then.empty() && child.start(then))
      while (child.running()) sleep_ms(500);
    return 1;
  }
  if (!then.empty() && !child.start(then)) g_exit = true;
  int period_ms = hz > 0 ? (int)(1000.0 / hz) : 11;
  int64_t end = seconds > 0 ? now_ns() + (int64_t)(seconds * 1e9) : 0;
  while (!g_exit && (!end || now_ns() < end) && (then.empty() || child.running())) {
    h.service();
    for (auto &l : drivers) {
      h.current_driver = l.name;
      l.provider->RunFrame();
    }
    sleep_ms(period_ms);
  }
  g_exit = true;
  {
    std::lock_guard<std::mutex> g(h.m);
    for (auto &d : h.devices)
      if (d->activated) d->driver->Deactivate();
  }
  for (auto &l : drivers) l.provider->Cleanup();
  h.shutdown();
  logf("exit");
  return 0;
}
}  // namespace dh

int main(int argc, char **argv) { return dh::run(argc, argv); }
