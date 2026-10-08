# How EAC runs in the guest

## The chain

```
ARM64 Steam (SteamOS guest)
 └─ Steam's FEX compatibility tool (app 3127680)   fex-compat-tool run --
     └─ Steam Linux Runtime 4 (_v2-entry-point --verb=run)
         └─ Proton Experimental (Wine 11.0, x86-64)
             └─ VRChat (Windows depot) + EAC Linux client (Proton EasyAntiCheat Runtime, app 1826330)
```

The x86-64 Proton runs under the FEX inside Steam's tool, so replacing that FEX replaces the emulator for the whole chain.
The guest is Valve's ARM64 SteamOS (Steam Frame image) with its rootfs unmodified; everything the setup adds lives in the
guest's home directory.

## Why the game needs Proton Experimental

On ARM64 Steam, VRChat installs the Android build (`VRChat.apk`, run by Valve's "Lepton" layer) by default. No FEX change
affects it. Forcing the compatibility tool to **Proton Experimental** selects the Windows depot (build 25738324), the build
with the EAC Linux client. The Proton EasyAntiCheat Runtime must be installed explicitly.

## Why Valve's FEX is not enough

EAC's Linux launcher injects its client into the game with `ptrace` and depends on Linux signal semantics. Valve's
FEX-2607-76-g37265b1 and stock FEX-2610 fail the research repo's three fidelity tests in the guest: `ptrace-inject` hangs
after `PASS: fork`, `signal-mask` fails, `signal-regs` fails (tgkill/kill/tkill). The launcher then never finishes and the
session never authenticates.

## What the patched FEX does

FEX-2610 with the patches in `fex-eac/patches/` (built in the guest with `-DBUILD_STEAM_SUPPORT=ON`) emulates the x86-64 view of a `ptrace`
tracee, runs guest signal handlers with the Linux signal mask, and keeps registers set on syscall-entered handlers; patch
0002 stops pages that keep faulting on self-modifying code from being write-protected. All three tests pass. Patch
descriptions and reasoning: [patches.md](https://github.com/StonkGuy/eac-arm64-emulation-poc/blob/master/docs/patches.md)
in the research repo.

| patch | what it is |
|---|---|
| 0001 | emulates the x86-64 view of a `ptrace` tracee, so the EAC launcher can inject its client |
| 0002 | stops write-protecting pages that keep faulting on self-modifying code |
| 0004 | cheaper code-range invalidation with ~200 threads |
| 0007 | runs guest signal handlers with the Linux signal mask |
| 0008 | keeps registers set on a handler entered from a syscall |
| 0010 | the emulated seccomp/SIGSYS path (`SECCOMP_RET_TRAP`) — a trapped syscall is entered after it, not re-run; today's Wine `install_bpf` mutual-trap livelock lives here |
| 0011–0021 | kernel-fidelity gaps a title or Wine can observe (debug registers, `/proc/<pid>/status`, `arch_prctl`, regsets, `restart_syscall`, the signal frame, a faulting `pop`) |
| 0003, 0005, 0006, 0009 | diagnostics and profiling only — not fixes |

Required for VRChat: 0001 (to inject at all), 0002 (so the client finishes loading) and 0007 (to avoid Photon
time-outs). 0008 is a correctness fix in the same area, 0004 is performance, 0010 is the seccomp path a title's
anti-tamper can drive, and 0003/0005/0006/0009 are instrumentation you can drop.

The install script swaps the binaries, `FEXCompatTool`, `VERSIONS.txt` and `ConfigTemplate.json` into the tool, backs up
Valve's copies and can restore them. FEX-2610's Steam config template enables DiskCache (a stock FEX option).

Nothing fakes or short-circuits EAC's result; the EAC client is the stock one from the Proton EAC Runtime.

## Hardware TSO

The guest kernel (Linux 7.2.9-steamac, 4 KB pages) logs `CPU features: detected: TSO memory model (Apple)`.
`prctl(PR_SET_MEM_MODEL, TSO)` succeeds, and `FEXGetConfig --tso-emulation-info` reports Hardware TSO for GPR, memcpy and
vector memory ordering, so FEX pays no software cost for x86 memory ordering.
