# Creating the SteamOS disk without Docker

The app user does not need Docker: the launcher creates the disk itself — through the first-launch
window's **Create New Disk…** or **Settings → Advanced → Create New Disk…** (stable/rc
branch, home size, location, password for user `steamos`; progress, Stop, and Resume). Only stable
and rc are offered: beta/preview/main may use Valve's development signing key, which is not trusted
by the launcher. A saved unsupported branch falls back to stable and is logged. The same
without a window:

```sh
work/out/steamac-vm --create-disk ~/steamos.img [--branch stable] [--home-gib 64] [--password PW] [--keep-cache] [--accept-eula]
```

Nothing is downloaded until the user accepts Valve's terms: “End User License Agreement for
SteamOS and Steam Client Back-Up Image” (the same text as on the Steam Frame image page,
`https://store.steampowered.com/steamos/download/?ver=steamframe`: personal use only, no
redistribution) and the Steam Subscriber Agreement. In the window this is a checkbox with links
to both texts — Create is unavailable without it; on the command line it is `--accept-eula`,
without which `--create-disk` prints the links and exits with code 2. Acceptance (date and
agreement URL) is stored in the settings domain and remains valid until the agreement URL in the
code (`SteamOSLicense.eulaURL`) changes.

External APFS, Mac OS Extended, and exFAT volumes can hold the disk. FAT32/MS-DOS is rejected
before downloading because of its 4 GiB per-file limit (the temporary rootfs alone is 10 GiB).
Read-only volumes and folders you can't write to are also rejected. Unlike APFS, exFAT has no sparse files: it needs space for the
full selected disk size plus the temporary rootfs and download cache, even before games are installed;
the launcher checks this space before reconstructing the rootfs.
These expected rejections (including an existing destination file or a busy download cache) are logged, not sent as errors
to Sentry. Unexpected creation failures still report: Foundation errors group by domain and code,
with the original technical diagnostic in the event details rather than pointers/task IDs in the title.

If a download cannot reach Valve securely, the window explains that a VPN, proxy, or network filter
may be interfering: try disabling it or using another network. Update checks give the same advice
for GitHub. Technical details stay in the launcher log. HTTPS uses macOS's standard certificate
validation and TLS settings; the pinned Valve CA below verifies the downloaded bundle, not HTTPS.

`rc` stays available, but Valve sometimes signs its latest build with the development key
`steamos-dev-images` instead of the production CA. That build is not accepted: the launcher explains
that it cannot verify this development signature and asks you to choose `stable` or try again later.
This expected rejection is logged only; all other signature failures still report to Sentry.

1. `https://steamdeck-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr/<branch>.json` → the latest
   candidate (`update_path`, `chunks_store_path`).
2. The `.raucb` (~2 MB) is downloaded; Security.framework verifies its CMS signature only against
   Valve's pinned CA `CN=steamdeck-images` (`scripts/keys/steamdeck-images.pem`, SHA-256 fingerprint
   embedded in the code); the system trust store is not used. A custom squashfs reader (using zstd
   from the pinned zstd release, `fetch-zstd.sh`) extracts `manifest.raucm` and `rootfs.img.caibx`;
   `compatible=steamos-aarch64`, the version, and the slot size are checked.
3. Official desync (`fetch-desync.sh`, pinned version and sha256) assembles the 10 GB `rootfs.img`
   from Valve's chunk stores (~4.4 GB of data); the chunk cache is `desync/` in
   `~/Library/Caches/es.fxgam.steamac` for a disk on the home volume, otherwise in `<disk>.cache` next
   to the disk (an external drive then needs no internal space for it; the folder is removed after
   success). The partial `<disk>.rootfs-tmp` remains too, so Stop/Resume (or rerunning the command
   after Ctrl+C) continues where it left off. One creation per cache at a time: a second one (another
   window or `--create-disk`) stops with “another SteamOS disk is being created” (`flock` on
   `creation.lock` in the cache folder) instead of sharing the chunk cache and temporary files.
4. A sparse disk file: protective MBR + GPT (primary and backup, CRC32) with exactly the names,
   order, types, sizes, and alignment of `scripts/steps/40-disk.sh`, and random PARTUUIDs. In one
   pass, `rootfs.img` is hashed (sha256 must match the signed manifest), and nonzero blocks of 16
   KiB are written to rootfs-A and rootfs-B; the other partitions are zeros. The disk appears
   under its final name only after all checks. Existing files are not overwritten; on exFAT,
   which lacks atomic exclusive rename, the launcher checks the destination while holding its
   creation lock, then renames it. Do not create or move another file to that same destination
   with a non-launcher program during creation: that check and rename are not atomic against it.
5. `<disk without .img>.provision.img` is placed alongside it — cpio newc containing
   `provision.env` (build, PARTUUIDs, SHA-512 crypt password hash, machine-id) and `rootfs.caibx`
   (format: “Payload v1” in the provisioning contract). While this file exists, the launcher
   attaches it read-only (vdc) and adds `steamac.provision=1`: initramfs formats
   esp/efi-X/var-X/home, makes the rootfs-B fsid unique, writes partsets/bootconf/bootenv/var,
   and reports `provision done` — the launcher then removes the payload; subsequent boots do not
   use it.

Space: ~14 GB on the disk volume during creation (~9 GB afterward), ~6 GB of cache (deleted after
success unless `--keep-cache` is specified). Checks: `work/out/steamac-vm --selftest-provision` —
GPT against the Docker-built disk (`work/out/steamos.img` is opened read-only;
`--reference-disk IMG`), CMS/squashfs against the `work/cache/rootfs` cache, cpio, SHA-512 crypt.
The self-test also checks network messages, stable error fingerprints and log-only location
rejections. To exercise an actual TLS failure without sending any events, point
`STEAMAC_PROVISION_TEST_TLS_URL=https://localhost:PORT/` at a local server with an untrusted
certificate when running `--selftest-provision`; it prints the message, title and fingerprint.

