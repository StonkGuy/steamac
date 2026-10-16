# Release builds (GitHub Actions)

`.github/workflows/release.yml` builds the app from a tag with this repository's own scripts and attaches
three files to the tag's GitHub release:

| file | what |
|---|---|
| `FX-Steam-Launcher-<tag>-adhoc.dmg` | the app, signed ad hoc (`DIST_SIGN=adhoc`, see below) |
| `FX-Steam-Launcher-<tag>-adhoc.dmg.sha256` | its SHA-256 |
| `FX-Steam-Launcher-<tag>-gpl-sources.tar` | corresponding source of the GPL/LGPL parts (`scripts/gpl-sources.sh`) |

It runs when a `v*` tag is pushed, or by hand: Actions > **Release DMG** > Run workflow, with `ref` (the tag,
branch or commit to build) and optionally `release_tag` (an existing release to attach the files to; empty keeps
them as a workflow artifact only). It has no `pull_request` trigger, so code from a fork's pull request never runs
with this repository's token. Only the last job (`release`) gets `contents: write`; every other job reads.
No other secret is used.

## Jobs

GitHub's arm64 macOS runners cannot run Linux containers (no nested virtualization), and the guest images are
built in arm64 Linux containers. The build is therefore split over two kinds of runner:

| job | runner | steps (the scripts `./build.sh` runs) |
|---|---|---|
| `dxmt` | `macos-26`, Xcode 26.6 | `host/dxmt/build.sh fetch llvm native host pe` |
| `guest` | `ubuntu-24.04-arm` | `guest/kernel/build.sh`, `scripts/build-image.sh builder rootfs`, `guest/mesa/build.sh`, `scripts/build-image.sh initramfs` |
| `layer` | `ubuntu-24.04-arm` | `host/dxmt/build.sh fetch client payload`, `scripts/build-image.sh builder layer`, `scripts/guest-toolchain.sh` |
| `app` | `macos-26`, Xcode 26.6 | `./build.sh host` (MoltenVK, KosmicKrisp, libepoxy, virglrenderer, libkrun, launcher, `bundle.sh`), launcher self-tests, `DIST_SIGN=adhoc host/launcher/dist.sh`, `scripts/test/dist-verify.sh --receipts` |
| `release` | `ubuntu-24.04` | `gh release upload` |

The x86 Mesa build and the DXMT guest client run in `linux/amd64` containers under QEMU user emulation on the
arm64 Linux runner, as they do under Rosetta in OrbStack on a Mac. The Mac's own build runs everything on one
machine; nothing here changes that.

## GPU tests

The macOS runners are virtual machines whose GPU is a paravirtual Metal device. It lacks features that every
Apple-silicon Mac has (mesh shaders, which MoltenVK's geometry shaders use; Metal 4 for KosmicKrisp). On a
GitHub-hosted runner the workflow therefore sets `MVK_SKIP_REPRO=1` (`host/moltenvk/build.sh`: the probe still runs, the repros
do not) and `KK_SKIP_HOST_TESTS=1` (`host/kosmickrisp/build.sh`). Before a DMG is published, the skipped suites
run on a Mac against the dylibs inside it:

```sh
hdiutil attach -readonly FX-Steam-Launcher-<tag>-adhoc.dmg
fw="/Volumes/FX Steam Launcher/FX Steam Launcher.app/Contents/Frameworks"
host/moltenvk/repro/run.sh "$fw"                                     # MoltenVK
REPRO_DRIVER=kosmickrisp host/moltenvk/repro/run.sh "$fw/libvulkan_kosmickrisp.dylib"
```

## Why the guest images can move between machines

The guest receipts (`scripts/guest-artifacts.py`, `work/out/*.inputs.json`) record a hash of the selected
source files and a hash of the output bytes. Neither contains a path or a machine name. The `app` job unpacks
the images built by `guest`/`layer` into its own checkout of the same commit. `bundle.sh` and `dist.sh` then check
the receipts against that checkout's sources and the unpacked bytes, as on a single machine. A changed source
file or a changed byte on the way fails the build there.

Two facts that `host/launcher/licenses.sh` reads from the build containers (the guest Rust version and the
builder image's busybox-static version and copyright) are written by `scripts/guest-toolchain.sh` on the
Linux runner. They travel with the images (`work/out/guest-toolchain.json`). `licenses.sh` reads that record only
on a host without Docker, and only if it names the same pinned images as `scripts/config.env`.

## Caches

LLVM 15 (`dxmt`, about 30 min), the kernel and Mesa (`guest`) are cached under keys made from their pins and build
scripts. A cache is saved as soon as its step succeeds. A restored kernel or Mesa counts only if its receipt
still matches the sources (`guest-artifacts.py check`); otherwise it is rebuilt. Valve's SteamOS rootfs (10 GiB,
needed only to verify Mesa) is not cached.

## Building an older tag

A tag older than the workflow is built by running the workflow from `main` with `ref` set to the tag. The
packaging scripts and the one test the tag lacks or has in an older form (`PACKAGING` in the workflow: `dist.sh`,
`licenses.sh`, `host/dxmt/build.sh`, `guest-toolchain.sh`, `dist-verify.sh`, `wmtr/tests/wmtr_loopback.m`) are
taken from the workflow's commit, and the log names each one. None of them is an input of a guest receipt or part
of the app's code.

## Ad hoc signature

`DIST_SIGN=adhoc host/launcher/dist.sh` signs the app as `bundle.sh` does for a local build (`codesign -s -`,
with the entitlements in `host/launcher/steamac-vm.entitlements`: hypervisor, disable-library-validation,
audio-input) and builds the same DMG, unsigned and not notarized. No Apple Developer account is involved.
macOS does not open such a download by itself. After the first attempt to open the app, allow it once in
System Settings > Privacy & Security > **Open Anyway**. Compare the DMG with its `.sha256` before that:
`shasum -a 256 -c FX-Steam-Launcher-<tag>-adhoc.dmg.sha256`.

`scripts/test/dist-verify.sh <dmg> [adhoc|developer-id]` checks a built DMG without starting a VM: layout,
`codesign --verify --deep --strict`, each Mach-O's signature kind, the entitlements, the Info.plist build identity,
and that no file names a `/Users/<name>` other than the build runner's. With `--receipts` it also checks the
bundled kernel, initramfs and layer against their receipts and the checkout.
