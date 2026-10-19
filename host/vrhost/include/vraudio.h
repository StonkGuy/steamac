/* SPDX-License-Identifier: Apache-2.0 OR GPL-3.0-or-later */
/*
 * VR audio region: game audio steamac-vm (libkrun virtio-snd) -> steamac-vrhost, headset microphone back.
 * Contract: docs/vr-interfaces.md section 6. Plain C so both sides can include it (libkrun mirrors it in Rust,
 * src/devices/src/virtio/snd/audio_backends/vr_audio.rs, with a layout test).
 *
 * One shared-memory region (a shm_open'ed and unlinked object, or any mmap-able fd), created by steamac-vm with
 * vra_region_init(), mapped by libkrun (krun_snd_set_vr_audio) and by steamac-vrhost (--audio-fd). No locks and
 * no wake-ups cross the boundary: two single-producer single-consumer rings of interleaved S16LE frames, each side
 * polls on its own clock (libkrun: the CoreAudio render callback / the virtio-snd pump; vrhost: a 5 ms tick, the
 * WiVRn audio quantum).
 *
 * Routing follows the headset, not the attachment: vrhost publishes in `headset` what the connected headset offers
 * and proves it is alive with `beat`. libkrun copies playback into the play ring (and, if asked, silences the Mac
 * output) only while VRA_HEADSET_SPEAKER is set, and records from the mic ring instead of the Mac microphone only
 * while VRA_HEADSET_MIC is set; without a headset, or with a vrhost that died, the guest's audio is the Mac's again.
 *
 * write/read count frames ever written/read (never wrap; index = count & (capacity - 1)). Producer: fill the
 * slots, then store write with release order. Consumer: load write with acquire order, copy, then store read with
 * release order. A producer that finds the ring full drops the new frames and counts them in overruns.
 */
#ifndef STEAMAC_VRAUDIO_H
#define STEAMAC_VRAUDIO_H

#include <stdint.h>
#include <string.h>

#define VRA_MAGIC 0x31415256u /* "VRA1" */
#define VRA_VERSION 1u
#define VRA_PLAY_CHANNELS 2u  /* FL, FR */
#define VRA_MIC_CHANNELS 1u   /* mono (WiVRn clients always report 1) */
#define VRA_PLAY_FRAMES 8192u /* 170 ms at 48 kHz; power of two */
#define VRA_MIC_FRAMES 8192u
#define VRA_HEADSET_SPEAKER 1u /* the play ring is being forwarded to a headset */
#define VRA_HEADSET_MIC 2u     /* the headset has a microphone: the mic ring is fed while mic.active */
#define VRA_BEAT_TIMEOUT_MS 1000u

struct vra_ring {
  uint64_t write;      /* producer */
  uint8_t pad0[56];
  uint64_t read;       /* consumer */
  uint8_t pad1[56];
  uint32_t rate;       /* producer: sample rate of the frames it writes (Hz), 0 = unknown yet */
  uint32_t channels;   /* fixed: VRA_PLAY_CHANNELS / VRA_MIC_CHANNELS */
  uint32_t capacity;   /* frames, power of two */
  uint32_t active;     /* mic ring: 1 while the guest records (written by libkrun); play ring: unused */
  uint64_t overruns;   /* producer: frames dropped, ring full */
  uint64_t underruns;  /* consumer: frames it had to make up (silence) */
  uint8_t pad2[32];
};

struct vra_region {
  uint32_t magic;   /* VRA_MAGIC */
  uint32_t version; /* VRA_VERSION */
  uint32_t size;    /* sizeof(struct vra_region) */
  uint32_t headset; /* vrhost: VRA_HEADSET_* of the connected headset, 0 while none is connected */
  uint32_t beat;    /* vrhost: incremented on every tick; libkrun ignores `headset` once it stood still for 1 s */
  uint8_t pad[44];
  struct vra_ring play; /* libkrun -> vrhost: guest playback, VRA_PLAY_CHANNELS, rate = guest stream rate */
  struct vra_ring mic;  /* vrhost -> libkrun: headset microphone, VRA_MIC_CHANNELS, rate = headset mic rate */
  int16_t play_data[VRA_PLAY_FRAMES * VRA_PLAY_CHANNELS];
  int16_t mic_data[VRA_MIC_FRAMES * VRA_MIC_CHANNELS];
};

#ifdef __cplusplus
static_assert(sizeof(struct vra_ring) == 192, "vra_ring layout");
static_assert(sizeof(struct vra_region) == 64 + 2 * 192 + 2 * (VRA_PLAY_FRAMES * 2 + VRA_MIC_FRAMES), "vra_region layout");
#else
_Static_assert(sizeof(struct vra_ring) == 192, "vra_ring layout");
_Static_assert(sizeof(struct vra_region) == 64 + 2 * 192 + 2 * (VRA_PLAY_FRAMES * 2 + VRA_MIC_FRAMES), "vra_region layout");
#endif

/* Bytes to map (whole pages of 16 KiB). */
#define VRA_REGION_BYTES (((sizeof(struct vra_region)) + 16383u) & ~16383u)

/* The creator (steamac-vm) initialises a zeroed mapping before handing the fd to either side. */
static inline void vra_region_init(struct vra_region *r) {
  memset(r, 0, sizeof *r);
  r->magic = VRA_MAGIC;
  r->version = VRA_VERSION;
  r->size = (uint32_t)sizeof *r;
  r->play.channels = VRA_PLAY_CHANNELS;
  r->play.capacity = VRA_PLAY_FRAMES;
  r->mic.channels = VRA_MIC_CHANNELS;
  r->mic.capacity = VRA_MIC_FRAMES;
}

static inline int vra_region_valid(const struct vra_region *r) {
  return r->magic == VRA_MAGIC && r->version == VRA_VERSION && r->size == sizeof *r &&
         r->play.channels == VRA_PLAY_CHANNELS && r->play.capacity == VRA_PLAY_FRAMES &&
         r->mic.channels == VRA_MIC_CHANNELS && r->mic.capacity == VRA_MIC_FRAMES;
}

#endif
