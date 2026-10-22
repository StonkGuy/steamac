// Copyright 2026 the steamac lean-graphics contributors
// SPDX-License-Identifier: BSD-3-Clause

//! wmtr: a virtio-gpu context type that carries DXMT's winemetal unix calls from a guest process
//! to a host executor (libwmtr_host.dylib, loaded at runtime) that runs them against Metal.
//!
//! Guest protocol (see wmtr_vgpu.h): blobs are VIRTGPU_BLOB_MEM_HOST3D with blob_id = wmtr blob
//! number; this context backs each one with POSIX shared memory that is mapped both into the guest
//! (by the VMM's existing SHM blob path) and into this process for the executor. EXECBUFFER carries
//! 24-byte commands: RING (blob 0 is initialised: start the executor), KICK (wake it), WAIT (the
//! next fence on this ring completes when the reply slot or event queue reaches `seq`).
//!
//! The component only exists when STEAMAC_WMTR=1 and the library loads, so nothing changes for
//! guests that never ask for the capset.

use log::{error, info};
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::ffi::CString;
use std::sync::Arc;
use std::sync::Mutex;

use crate::rutabaga_core::RutabagaComponent;
use crate::rutabaga_core::RutabagaContext;
use crate::rutabaga_core::RutabagaResource;
use crate::rutabaga_os::FromRawDescriptor;
use crate::rutabaga_os::SafeDescriptor;
use crate::rutabaga_utils::*;

pub const RUTABAGA_CAPSET_WMTR: u32 = 30;

const WMTR_VGPU_RING: u32 = 1;
const WMTR_VGPU_KICK: u32 = 2;
const WMTR_VGPU_WAIT: u32 = 3;
const CMD_SIZE: usize = 24;

#[repr(C)]
struct WmtrHostOps {
    blob_ptr: extern "C" fn(*mut c_void, u32, *mut u64) -> *mut c_void,
    wake: extern "C" fn(*mut c_void, u32),
    create_view: Option<extern "C" fn(*mut c_void, u64, *mut c_void, *mut u64) -> u64>,
    release_view: Option<extern "C" fn(*mut c_void, u64)>,
    map_path: Option<extern "C" fn(*mut c_void, *const i8, *mut i8, u64) -> *const i8>,
}

type HostCreate = unsafe extern "C" fn(*mut c_void, *const WmtrHostOps, *mut c_void) -> *mut c_void;
type HostVoid = unsafe extern "C" fn(*mut c_void);
type HostProgress = unsafe extern "C" fn(*mut c_void, u32) -> u64;
type ViewCreate = extern "C" fn(*mut c_void, u64, *mut c_void, *mut u64) -> u64;
type ViewRelease = extern "C" fn(*mut c_void, u64);
type MapPath = extern "C" fn(*mut c_void, *const i8, *mut i8, u64) -> *const i8;

#[derive(Clone, Copy)]
struct HostLib {
    create: HostCreate,
    kick: HostVoid,
    destroy: HostVoid,
    progress: HostProgress,
    create_view: Option<ViewCreate>,
    release_view: Option<ViewRelease>,
    map_path: Option<MapPath>,
}

unsafe impl Send for HostLib {}
unsafe impl Sync for HostLib {}

fn load_host_lib() -> Option<HostLib> {
    let name = std::env::var("STEAMAC_WMTR_LIB").unwrap_or_else(|_| "libwmtr_host.dylib".into());
    let cname = CString::new(name.clone()).ok()?;
    // SAFETY: dlopen/dlsym on a library we built; symbols are checked for null before use.
    unsafe {
        let h = libc::dlopen(cname.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
        if h.is_null() {
            error!("wmtr: cannot load {name}");
            return None;
        }
        let sym = |s: &str| -> *mut c_void {
            let c = CString::new(s).unwrap();
            libc::dlsym(h, c.as_ptr())
        };
        let (c, k, d, p) = (
            sym("wmtr_host_create"),
            sym("wmtr_host_kick"),
            sym("wmtr_host_destroy"),
            sym("wmtr_host_progress"),
        );
        if c.is_null() || k.is_null() || d.is_null() || p.is_null() {
            error!("wmtr: {name} lacks the host API");
            return None;
        }
        let opt = |s: &str| {
            let p = sym(s);
            if p.is_null() {
                None
            } else {
                Some(p)
            }
        };
        Some(HostLib {
            create: std::mem::transmute::<*mut c_void, HostCreate>(c),
            kick: std::mem::transmute::<*mut c_void, HostVoid>(k),
            destroy: std::mem::transmute::<*mut c_void, HostVoid>(d),
            progress: std::mem::transmute::<*mut c_void, HostProgress>(p),
            create_view: opt("wmtr_present_create_view")
                .map(|p| std::mem::transmute::<*mut c_void, ViewCreate>(p)),
            release_view: opt("wmtr_present_release_view")
                .map(|p| std::mem::transmute::<*mut c_void, ViewRelease>(p)),
            map_path: opt("wmtr_map_path").map(|p| std::mem::transmute::<*mut c_void, MapPath>(p)),
        })
    }
}

struct Waiter {
    fence: RutabagaFence,
    slot: u32,
    seq: u64,
}

/// State shared with the executor's callbacks (which run on executor/dispatch threads).
struct Shared {
    lib: HostLib,
    blobs: Mutex<BTreeMap<u32, (usize, u64)>>,
    waiters: Mutex<Vec<Waiter>>,
    host: Mutex<usize>,
    fence_handler: RutabagaFenceHandler,
}

extern "C" fn cb_blob_ptr(ctx: *mut c_void, blob: u32, size: *mut u64) -> *mut c_void {
    // SAFETY: ctx is the Shared the context keeps alive until the executor is destroyed.
    let s = unsafe { &*(ctx as *const Shared) };
    match s.blobs.lock().unwrap().get(&blob) {
        Some(&(p, n)) => {
            // SAFETY: size points to the caller's u64.
            unsafe { *size = n };
            p as *mut c_void
        }
        None => std::ptr::null_mut(),
    }
}

extern "C" fn cb_wake(ctx: *mut c_void, slot: u32) {
    // SAFETY: as above.
    let s = unsafe { &*(ctx as *const Shared) };
    s.signal_ready(Some(slot));
}

impl Shared {
    fn progress(&self, slot: u32) -> u64 {
        let host = *self.host.lock().unwrap();
        if host == 0 {
            return 0;
        }
        // SAFETY: host is a live executor handle.
        unsafe { (self.lib.progress)(host as *mut c_void, slot) }
    }

    /// Completes every waiter (of `slot`, or all) whose condition holds.
    fn signal_ready(&self, slot: Option<u32>) {
        let mut w = self.waiters.lock().unwrap();
        let mut i = 0;
        while i < w.len() {
            if slot.is_none_or(|s| s == w[i].slot) && self.progress(w[i].slot) >= w[i].seq {
                let x = w.swap_remove(i);
                self.fence_handler.call(x.fence);
            } else {
                i += 1;
            }
        }
    }
}

pub struct WmtrContext {
    shared: Arc<Shared>,
    ops: Box<WmtrHostOps>,
    pending: BTreeMap<u8, (u32, u64)>,
    maps: Vec<(usize, u64)>,
}

// SAFETY: the raw pointers are only host addresses of mappings this context owns.
unsafe impl Send for WmtrContext {}

impl WmtrContext {
    fn host(&self) -> *mut c_void {
        *self.shared.host.lock().unwrap() as *mut c_void
    }
}

impl RutabagaContext for WmtrContext {
    fn context_create_blob(
        &mut self,
        resource_id: u32,
        resource_create_blob: ResourceCreateBlob,
        _handle_opt: Option<RutabagaHandle>,
    ) -> RutabagaResult<RutabagaResource> {
        let blob = resource_create_blob.blob_id as u32;
        let size = resource_create_blob.size;
        if size == 0 || blob as usize >= 1024 {
            return Err(RutabagaError::SpecViolation("wmtr: bad blob"));
        }
        let name = CString::new(format!("/wmtr.{}.{}.{}", std::process::id(), resource_id, blob)).unwrap();
        // SAFETY: plain POSIX shm calls; every result is checked.
        let (fd, ptr) = unsafe {
            let fd = libc::shm_open(name.as_ptr(), libc::O_RDWR | libc::O_CREAT | libc::O_EXCL, 0o600);
            if fd < 0 {
                return Err(RutabagaError::SpecViolation("wmtr: shm_open failed"));
            }
            libc::shm_unlink(name.as_ptr());
            if libc::ftruncate(fd, size as libc::off_t) != 0 {
                libc::close(fd);
                return Err(RutabagaError::SpecViolation("wmtr: ftruncate failed"));
            }
            let p = libc::mmap(
                std::ptr::null_mut(),
                size as usize,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            );
            if p == libc::MAP_FAILED {
                libc::close(fd);
                return Err(RutabagaError::SpecViolation("wmtr: mmap failed"));
            }
            (fd, p as usize)
        };
        self.shared.blobs.lock().unwrap().insert(blob, (ptr, size));
        self.maps.push((ptr, size));
        // SAFETY: fd is a fresh descriptor we own.
        let os_handle = unsafe { SafeDescriptor::from_raw_descriptor(fd) };
        Ok(RutabagaResource {
            resource_id,
            handle: Some(Arc::new(RutabagaHandle {
                os_handle,
                handle_type: RUTABAGA_MEM_HANDLE_TYPE_SHM,
            })),
            blob: true,
            blob_mem: resource_create_blob.blob_mem,
            blob_flags: resource_create_blob.blob_flags,
            map_info: Some(RUTABAGA_MAP_CACHE_CACHED | RUTABAGA_MAP_ACCESS_RW),
            info_2d: None,
            info_3d: None,
            vulkan_info: None,
            backing_iovecs: None,
            component_mask: 1 << (RutabagaComponentType::Wmtr as u8),
            size,
            mapping: None,
        })
    }

    fn submit_cmd(&mut self, commands: &mut [u8], _fence_ids: &[u64]) -> RutabagaResult<()> {
        for c in commands.chunks_exact(CMD_SIZE) {
            let ty = u32::from_le_bytes(c[0..4].try_into().unwrap());
            let slot = u32::from_le_bytes(c[4..8].try_into().unwrap());
            let seq = u64::from_le_bytes(c[8..16].try_into().unwrap());
            match ty {
                WMTR_VGPU_RING => {
                    if !self.host().is_null() {
                        return Err(RutabagaError::SpecViolation("wmtr: ring already started"));
                    }
                    let ring = match self.shared.blobs.lock().unwrap().get(&0) {
                        Some(&(p, _)) => p,
                        None => return Err(RutabagaError::SpecViolation("wmtr: no ring blob")),
                    };
                    let ctx = Arc::as_ptr(&self.shared) as *mut c_void;
                    // SAFETY: ring is our mapping of blob 0; ops and ctx outlive the executor
                    // (destroyed in Drop before they are freed).
                    let h = unsafe { (self.shared.lib.create)(ring as *mut c_void, &*self.ops, ctx) };
                    if h.is_null() {
                        return Err(RutabagaError::SpecViolation("wmtr: executor start failed"));
                    }
                    *self.shared.host.lock().unwrap() = h as usize;
                }
                WMTR_VGPU_KICK => {
                    let h = self.host();
                    if !h.is_null() {
                        // SAFETY: live executor.
                        unsafe { (self.shared.lib.kick)(h) };
                    }
                }
                WMTR_VGPU_WAIT => {
                    let ring = if slot >= 64 { 63 } else { 1 + (slot % 62) as u8 };
                    self.pending.insert(ring, (slot, seq));
                }
                _ => return Err(RutabagaError::SpecViolation("wmtr: unknown command")),
            }
        }
        Ok(())
    }

    fn attach(&mut self, _resource: &mut RutabagaResource) {}

    fn detach(&mut self, _resource: &RutabagaResource) {}

    fn context_create_fence(&mut self, fence: RutabagaFence) -> RutabagaResult<()> {
        let Some((slot, seq)) = self.pending.remove(&fence.ring_idx) else {
            self.shared.fence_handler.call(fence);
            return Ok(());
        };
        // Check and enqueue under the waiter lock: the executor publishes progress before it
        // takes this lock in cb_wake, so a reply cannot slip between the check and the push.
        let mut w = self.shared.waiters.lock().unwrap();
        if self.shared.progress(slot) >= seq {
            drop(w);
            self.shared.fence_handler.call(fence);
        } else {
            w.push(Waiter { fence, slot, seq });
        }
        Ok(())
    }

    fn component_type(&self) -> RutabagaComponentType {
        RutabagaComponentType::Wmtr
    }
}

impl Drop for WmtrContext {
    fn drop(&mut self) {
        let h = self.host();
        if !h.is_null() {
            // SAFETY: live executor; destroy joins its thread.
            unsafe { (self.shared.lib.destroy)(h) };
            *self.shared.host.lock().unwrap() = 0;
        }
        // complete anything still waiting so guest pollers do not hang
        for w in self.shared.waiters.lock().unwrap().drain(..) {
            self.shared.fence_handler.call(w.fence);
        }
        for &(p, n) in &self.maps {
            // SAFETY: our own mappings.
            unsafe { libc::munmap(p as *mut c_void, n as usize) };
        }
    }
}

pub struct Wmtr {
    lib: HostLib,
    fence_handler: RutabagaFenceHandler,
}

impl Wmtr {
    /// Returns the component when STEAMAC_WMTR=1 and the host library loads.
    pub fn init(fence_handler: RutabagaFenceHandler) -> Option<Box<dyn RutabagaComponent>> {
        if std::env::var("STEAMAC_WMTR").ok().as_deref() != Some("1") {
            return None;
        }
        let lib = load_host_lib()?;
        info!("wmtr: context type enabled (capset {RUTABAGA_CAPSET_WMTR})");
        Some(Box::new(Wmtr { lib, fence_handler }))
    }
}

impl RutabagaComponent for Wmtr {
    fn get_capset_info(&self, _capset_id: u32) -> (u32, u32) {
        (1, 16)
    }

    fn get_capset(&self, _capset_id: u32, _version: u32) -> Vec<u8> {
        // struct wmtr_vgpu_caps { u32 version, page_size; u64 max_blob; }
        let mut v = Vec::with_capacity(16);
        v.extend_from_slice(&1u32.to_le_bytes());
        v.extend_from_slice(&16384u32.to_le_bytes());
        v.extend_from_slice(&(4u64 << 30).to_le_bytes());
        v
    }

    fn create_fence(&mut self, fence: RutabagaFence) -> RutabagaResult<()> {
        self.fence_handler.call(fence);
        Ok(())
    }

    fn create_context(
        &self,
        _ctx_id: u32,
        _context_init: u32,
        _context_name: Option<&str>,
        fence_handler: RutabagaFenceHandler,
    ) -> RutabagaResult<Box<dyn RutabagaContext>> {
        let ops = Box::new(WmtrHostOps {
            blob_ptr: cb_blob_ptr,
            wake: cb_wake,
            create_view: self.lib.create_view,
            release_view: self.lib.release_view,
            map_path: self.lib.map_path,
        });
        Ok(Box::new(WmtrContext {
            shared: Arc::new(Shared {
                lib: self.lib,
                blobs: Mutex::new(BTreeMap::new()),
                waiters: Mutex::new(Vec::new()),
                host: Mutex::new(0),
                fence_handler,
            }),
            ops,
            pending: BTreeMap::new(),
            maps: Vec::new(),
        }))
    }
}

#[cfg(test)]
mod tests {
    //! Needs the built host library: STEAMAC_WMTR_LIB=/path/to/libwmtr_host.dylib (skipped when unset).
    use super::*;
    use crate::rutabaga_os::AsRawDescriptor;

    type Layout = unsafe extern "C" fn(*mut u64, *mut u64, *mut u64, *mut u64);

    fn layout(path: &str) -> (u64, u64, u64, u64) {
        let c = CString::new(path).unwrap();
        // SAFETY: test-only dlopen of the library under test.
        unsafe {
            let h = libc::dlopen(c.as_ptr(), libc::RTLD_NOW);
            assert!(!h.is_null());
            let f: Layout = std::mem::transmute(libc::dlsym(h, c"wmtr_layout".as_ptr()));
            let (mut a, mut b, mut cc, mut d) = (0, 0, 0, 0);
            f(&mut a, &mut b, &mut cc, &mut d);
            (a, b, cc, d)
        }
    }

    /// Blob 0 through context_create_blob, executor start (RING), one synchronous PING request written
    /// into the ring by hand, KICK, and a WAIT fence that must complete exactly when the reply lands.
    #[test]
    fn ring_ping_completes_wait_fence() {
        let Ok(lib) = std::env::var("STEAMAC_WMTR_LIB") else {
            eprintln!("STEAMAC_WMTR_LIB unset: skipped");
            return;
        };
        std::env::set_var("STEAMAC_WMTR", "1");
        let (ring_bytes, data_off, slots_off, slot_size) = layout(&lib);
        let done: Arc<Mutex<Vec<RutabagaFence>>> = Arc::new(Mutex::new(Vec::new()));
        let d2 = done.clone();
        let handler = RutabagaFenceHandler::new(move |f| d2.lock().unwrap().push(f));
        let comp = Wmtr::init(handler.clone()).expect("component");
        let mut ctx = comp.create_context(1, RUTABAGA_CAPSET_WMTR, None, handler).unwrap();
        let res = ctx
            .context_create_blob(
                1,
                ResourceCreateBlob { blob_mem: 2, blob_flags: 3, blob_id: 0, size: ring_bytes },
                None,
            )
            .unwrap();
        let fd = res.handle.as_ref().unwrap().os_handle.as_raw_descriptor();
        // SAFETY: maps the blob's shm like the guest does.
        let base = unsafe {
            libc::mmap(std::ptr::null_mut(), ring_bytes as usize, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, fd, 0)
        } as *mut u8;
        assert_ne!(base as *mut c_void, libc::MAP_FAILED);
        // SAFETY: offsets come from the host library's own layout.
        unsafe {
            *(base as *mut u32) = 0x52544d57; // magic
            *(base.add(4) as *mut u32) = 3; // version (WMTR_VERSION, wmtr_proto.h)
        }
        let cmd = |ty: u32, slot: u32, seq: u64| {
            let mut v = Vec::with_capacity(CMD_SIZE);
            v.extend_from_slice(&ty.to_le_bytes());
            v.extend_from_slice(&slot.to_le_bytes());
            v.extend_from_slice(&seq.to_le_bytes());
            v.extend_from_slice(&0u64.to_le_bytes());
            v
        };
        ctx.submit_cmd(&mut cmd(WMTR_VGPU_RING, 0, 0), &[]).unwrap();
        // WAIT slot 5 seq 1 on ring 6, fence before the request exists: must stay pending
        ctx.submit_cmd(&mut cmd(WMTR_VGPU_WAIT, 5, 1), &[]).unwrap();
        let fence = RutabagaFence { flags: RUTABAGA_FLAG_INFO_RING_IDX, fence_id: 77, ctx_id: 1, ring_idx: 6 };
        ctx.context_create_fence(fence).unwrap();
        assert!(done.lock().unwrap().is_empty(), "fence completed before the reply");
        // request: wmtr_req_ext {size 40, call PING=3, flags SYNC|CONTROL, slot 5, params 8, seq 1, 0 relocs, 0 outs, payload 8}
        let mut req = Vec::new();
        req.extend_from_slice(&40u32.to_le_bytes());
        req.extend_from_slice(&3u16.to_le_bytes());
        req.extend_from_slice(&5u16.to_le_bytes());
        req.extend_from_slice(&5u32.to_le_bytes());
        req.extend_from_slice(&8u32.to_le_bytes());
        req.extend_from_slice(&1u64.to_le_bytes());
        req.extend_from_slice(&0u16.to_le_bytes());
        req.extend_from_slice(&0u16.to_le_bytes());
        req.extend_from_slice(&8u32.to_le_bytes());
        req.extend_from_slice(&0u64.to_le_bytes()); // payload
        assert_eq!(req.len(), 40);
        // SAFETY: as above; head is at offset 8 of struct wmtr_ring, waiting flag at +16 of a slot.
        unsafe {
            std::ptr::copy_nonoverlapping(req.as_ptr(), base.add(data_off as usize), 40);
            *(base.add((slots_off + 5 * slot_size + 16) as usize) as *mut u32) = 1; // slot 5 waiting
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
            *(base.add(8) as *mut u64) = 40; // head
        }
        ctx.submit_cmd(&mut cmd(WMTR_VGPU_KICK, 0, 0), &[]).unwrap();
        for _ in 0..2000 {
            if !done.lock().unwrap().is_empty() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        let got = done.lock().unwrap().clone();
        assert_eq!(got.len(), 1, "WAIT fence not completed by the reply");
        assert_eq!(got[0].fence_id, 77);
        // SAFETY: done_seq is the first field of slot 5.
        assert_eq!(unsafe { *(base.add((slots_off + 5 * slot_size) as usize) as *const u64) }, 1);
        // a fence on a ring with no pending WAIT completes at once
        ctx.context_create_fence(RutabagaFence { flags: RUTABAGA_FLAG_INFO_RING_IDX, fence_id: 78, ctx_id: 1, ring_idx: 0 }).unwrap();
        assert_eq!(done.lock().unwrap().len(), 2);
        drop(ctx);
    }
}
