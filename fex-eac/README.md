# EAC in the SteamOS guest via Valve's FEX compat tool

Run Easy Anti-Cheat games (tested: VRChat) on a steamac Mac by replacing the FEX inside Steam's own FEX compatibility tool
(Steam app 3127680) with a FEX-2610 build carrying the patches in `patches/`.

EAC's Linux module runs the game under the x86-64 Proton that itself runs inside Steam's FEX tool. The module drives the
game through `ptrace` and depends on exact Linux signal semantics; Valve's FEX-2607 build fails that under emulation, so
the session never authenticates. The patched FEX passes the `ptrace` and signal-fidelity tests in the research repo below.

| patch | what it is |
|---|---|
| 0001 | `ptrace` emulation |
| 0002 | SMC hot pages |
| 0007, 0008, 0010 | signal mask, syscall-info and seccomp/SIGSYS fidelity (0010 also stops a `SECCOMP_RET_TRAP` filter from letting the trapped syscall run) |
| 0011–0020 | kernel-fidelity gaps a title or Wine can observe (debug registers, `/proc/<pid>/status`, `arch_prctl`, regsets, `restart_syscall`, the signal frame) |
| 0003–0006, 0009 | accounting and profiling only |

Required for VRChat: 0001, 0002 and 0007/0008. The rest (0003–0006, 0009) are accounting and profiling only.
`build-fex.sh` applies every patch in `patches/` (0001–0020) and pins base commit `14c92681`. Patch 0021 (`pop r/m`) was withdrawn — it broke the normal path and hung the EAC launcher; see `patches/withdrawn/`.

**Scope.** These patches do not try to make FEX a replica of native x86-64 Linux. Like upstream FEX, they make a
behaviour exact only where real software observes it (the anti-cheat launcher, Wine, a title's own probes) and keep it
free on paths that never use it. A real x86-64 kernel is the reference for what *correct* means for the behaviour a
patch touches, not a target for everything else; known differences stay listed as limits. The full note is in the
research repository's `docs/patches.md`.

### Games whose Wine seccomp filter install fails

Some titles make Wine's ntdll install a seccomp filter to trap guest system calls (`dlls/ntdll/unix/signal_x86_64.c`,
`install_bpf`). Where that install is rejected, Wine cannot arm its SIGSYS path and the workload that depends on it
(often an anti-tamper or anti-debug client) misbehaves — on one title the same program crashed at the `syscall`
instruction without the flag and livelocked inside FEX's code-invalidation lock with it. For such a game, set:

```
env FEX_NEEDSSECCOMP=1 %command%
```

`FEX_NEEDSSECCOMP` is a **stock FEX option**; the behaviour behind it (the emulated seccomp/SIGSYS path) is **patch
0010**. VRChat does not need the flag.

## What it takes

| you need | why |
|---|---|
| steamac 1.8.x on an Apple silicon Mac | verified: M2 MacBook Air, 16 GB, macOS 27.0 (other Macs and macOS versions are untested) |
| VRChat installed in the SteamOS guest, and Proton Experimental available as a compat tool | ARM64 Steam otherwise installs the Android build, which no FEX change can fix |
| the guest's own clang 19, cmake, ninja, lld, git, python3 | all shipped in the Steam Frame image, so FEX builds natively inside the guest |

## Steps

### 1. Steam setup

* Set VRChat's compatibility tool to **Proton Experimental**. ARM64 Steam otherwise installs the Android build for VRChat,
  which no FEX change can fix.
* Install **Proton EasyAntiCheat Runtime** (a Steam tool) for the compatdata prefix EAC uses.

### 2. Build and install the patched FEX

Enable SSH in the guest (Settings > Advanced > Enable SSH), copy `fex-eac/` over, then:

```sh
sh build-fex.sh            # clones FEX-2610, applies patches/, builds, stages into ~/fex-eac-work/stage
sh install-fex-tool.sh     # swaps the stage into the compat tool; backs up Valve's files first
sh install-fex-tool.sh status    # which build is installed
sh install-fex-tool.sh restore   # put Valve's build back
```

Optionally run `sudo sh guest-tune.sh` to drop the gamescope MangoApp overlay layer.

### 3. VRChat launch options

Steam > VRChat > Properties > Launch Options:

```
env EAC_LAUNCHERDIR=$HOME/.local/share/Steam/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat PROTON_EAC_RUNTIME='$HOME/.local/share/Steam/steamapps/common/Proton EasyAntiCheat Runtime' WINEDEBUG=-all PROTON_USE_XALIA=0 WINE_CPU_TOPOLOGY=16:0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3 %command%
```

`WINE_CPU_TOPOLOGY` reports 16 CPUs mapped onto the guest's 4 vCPUs; without it IL2CPP's thread pool sizes itself small and
the pre-join region lookup starves. Keep the count at 16 but list the guest's actual vCPU ids (see `nproc`) in place of the
repeated `0,1,2,3` above.

### 4. After a Steam update

A Steam update of the FEX tool overwrites it with Valve's build. Re-run `sh install-fex-tool.sh`.

## Results

Measured on an Apple M2 MacBook Air 16 GB, macOS 27.0, steamac 1.8.1/1.8.2, three sessions (2026-10-08).

| measurement | result |
|---|---|
| EAC launcher | `Launcher finished with: 301` in ~5 s, every session |
| anti-cheat session | `AntiCheat Session Begin: Success`, no `null client` |
| world entry | `Finished entering world`; no Photon time-out after |
| region lookup | 4–6 s |
| join → world | 86 s first run, 32 s second run |
| hardware TSO | active in the guest |

Patch details and the test suite: <https://github.com/StonkGuy/eac-arm64-emulation-poc>

## Optional: environment realism

`realism.sh` makes the guest look less like a microVM to software that inspects the machine's hardware identity. It
follows the hardware-realism advice in VRChat's *Using VRChat in a Virtual Machine* guide and duplicates the research
repo's `scripts/vm/realism.sh` (there, `REALISM=1 scripts/vm/steam-vm.sh`). It does not touch the anti-cheat or its
results.

**Off by default, and not used for the results above.** VRChat authenticates without it, so leave it off unless a
different game's anti-cheat rejects the VM and you are troubleshooting that.

```sh
sudo sh realism.sh          # apply (idempotent; re-run after a reboot, mounts are not persistent)
sudo sh realism.sh undo     # detach the mounts and restore the hostname
```

Override the values with `REALISM_*` variables, exactly as in the research repo — `REALISM_VENDOR`, `REALISM_PRODUCT`,
`REALISM_PRODUCT_VERSION`, `REALISM_BIOS_VENDOR`, `REALISM_BIOS_VERSION`, `REALISM_BIOS_DATE`, `REALISM_HOSTNAME`
(default `GAMING-PC`), and `REALISM_DIR` for the staging directory (default `/tmp/vrchat-fex-eac-realism`).

What it applies here differs from the research repo, because this guest is an aarch64 microVM booted from device tree
(`/sys/firmware/fdt`) with no SMBIOS and no PCI:

| part | script | this guest |
|---|---|---|
| DMI/SMBIOS strings | `realism.sh` | applied; as a read-only overlay, since the kernel exposes no DMI to replace |
| PCI device list | `realism.sh` | skipped — no `/proc/bus/pci` |
| PID 1 = systemd | `realism.sh` | cosmetic — the guest already runs systemd as PID 1 |
| hostname → `GAMING-PC` | `realism.sh` | applied |

### Hiding the hypervisor CPUID bit

FEX has a stock config option for the most common VM check, the hypervisor-present bit in CPUID leaf 1. It needs no
script and no code from this repo:

* `HideHypervisorBit` — `FEXCore/Source/Interface/Config/Config.json.in:701`, `bool`, default `false`, environment
  variable `FEX_HIDEHYPERVISORBIT` (FEX maps every option to `FEX_` + its uppercase enum name).
* Setting it clears **only** leaf 1 `ECX[31]` (`CPUID.cpp:452`, `CPUID.cpp:494`). It does **not** hide the FEX
  hypervisor leaves: CPUID `0x40000000` still returns with signature `FEXIFEXIEMU` (`CPUID.cpp:968`, `CPUID.cpp:984`).
  Anything that walks `0x40000000+` still sees FEX.

To use it, add `FEX_HIDEHYPERVISORBIT=1` to a game's launch options, e.g. in front of `%command%`. The variable is
documented in the config JSON source (`FEXCore/Source/Interface/Config/Config.json.in`) and in the man page the build
generates in its own tree; a `-DBUILD_STEAM_SUPPORT=ON` build does **not** install a man page into the compat tool, so
there is no `FEX.1` beside the binaries to read. `FEXGetConfig --version` confirms which build is installed.

## Notes

Not affiliated with Valve, Epic Games or VRChat. Standalone; nothing here is upstreamed.
