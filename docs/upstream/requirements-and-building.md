# Requirements

- Apple Silicon Mac, macOS 15+, ~150 GB of free space (the disk image is sparse).
- Xcode 26+ (full installation: its `actool` builds the app icon from an Icon Composer document), Homebrew, rustup.
- KosmicKrisp (optional, alternative Vulkan driver): built only on macOS 26+; `host/kosmickrisp/build.sh`
  installs its Homebrew dependencies (`llvm spirv-llvm-translator spirv-tools vulkan-loader glslang`).
- OrbStack (or Docker with arm64 and `--privileged`): the kernel, Mesa, and disk image are built in Linux containers.
- Homebrew packages: `meson ninja pkg-config dtc xz lld sshpass go` (`go`: license inventory of the bundled gvproxy/desync).
- libepoxy 1.5.10 is built from pinned sources by `host/libepoxy/build.sh`, before virglrenderer and
  libkrun; all three use `MACOSX_DEPLOYMENT_TARGET=15.0`, including on macOS 26/27 build hosts.
  Bundling rejects Mach-O deployment targets above macOS 15.0 (KosmicKrisp alone may require 26.0).

# Building and running

```sh
./build.sh          # everything: MoltenVK, virglrenderer, libkrun, launcher, kernel, Mesa, disk
./run.sh            # a window with SteamOS
```

Build parts separately: `./build.sh host`, `./build.sh guest`, or the scripts in the table below.
The SteamOS image is downloaded from Valve's servers (a signed RAUC bundle); its signature and
sha256 are verified.

Guest release artifacts have adjacent `.inputs.json` build receipts binding their selected Git
working-tree inputs to their output bytes. Relevant dirty edits invalidate them; unrelated commits
do not. `host/launcher/bundle.sh` refuses missing/stale receipts before modifying the app;
`dist.sh` also validates the receipts and resources inside an existing app before packaging it.
The error names the step to rerun. To refresh guest artifacts without touching an existing SteamOS
disk: `guest/kernel/build.sh` (only when its inputs changed or its receipt is missing),
`scripts/build-image.sh builder rootfs`, `guest/mesa/build.sh`, then
`scripts/build-image.sh initramfs layer`; finally `host/launcher/build.sh`. An unstamped artifact
needs its normal build once; kernel builds reuse the build volume without `clean`.
`ALLOW_NO_VENUS=1` layers are test-only and cannot be released.

VM options: `./run.sh --display 1920x1080 --cpus 10 --mem 24576` (full list:
`work/out/steamac-vm --help`).

Frame pacing: `./run.sh --perf-stats` (or `STEAMAC_PERF_STATS=1`) prints guest-frame and on-screen
frame intervals to the terminal every 5 s (p50/p95/p99/max, number of intervals > 25 and > 50 ms).
Stutters when a new effect first appears come from Metal shader compilation (~50–100 ms per
pipeline); Metal caches the result on disk, so the effect does not stutter again, even after
restarting the game or VM (measured via Venus: 102 ms → 0.95 ms per pipeline in a new process).

Steam Shader Pre-Caching and “Allow background processing of Vulkan shaders” (Settings → Downloads)
are on by default in the VM. Pre-caching is what brings Proton the transcoded cutscene videos: Steam
downloads them per game (`steamapps/shadercache/<appid>/transcoded_video.foz`, e.g. 836 MB for
Heroes of Might and Magic: Olden Era, 3.8 GB for Diplomacy is Not an Option) and passes
`STEAM_COMPAT_TRANSCODED_MEDIA_PATH` to Proton, which plays them for videos it cannot decode itself;
with pre-caching off those videos show a placeholder. DXVK's own cache (DXVK 2.7) lives in the game's
Wine prefix and works either way. The cost: every pipeline Steam's fossilize_replay processes is
compiled by Metal on the Mac (~70–100 ms), and Steam processes again after its shader updates for a
game and, for every game, after a launcher update that changes MoltenVK (the Venus driver identity is
a hash of MoltenVK's `pipelineCacheUUID`). Background processing does most of that while Steam is
idle; what is left shows as “Processing Vulkan shaders” when a game starts and can be skipped with
**Skip**. To turn it off: Steam → Settings → Downloads → Enable Shader Pre-caching (and/or Allow
background processing of Vulkan shaders).

Stock Steam keeps background processing off (`EnableShaderBackgroundProcessing` absent from
`~/.local/share/Steam/config/config.vdf` reads as 0). Before Steam starts,
`/usr/lib/steamac/steam-shader-defaults` (ExecStartPre of `steam.service`) writes
`"EnableShaderBackgroundProcessing" "1"` to the `ShaderCacheManager` block if there is no value yet,
once per Steam installation (marker `config/steamac-shader-defaults`), so a choice made in Steam's
settings stays. Launcher 1.4 shipped both settings off (`"DisableShaderCache" "1"`,
`"EnableShaderBackgroundProcessing" "0"`); on the first Steam start after updating, the script turns
both back on once, but only if both are still exactly those values.

SteamOS takes the Mac's time zone and 12/24-hour clock format (Settings > General
**Use the Mac's time zone and clock format**, on by default, applies on the next start).
On every boot the launcher adds `steamac.tz=<IANA zone of the Mac>` (`TimeZone.current`) and
`steamac.clock24=0|1` (the localized `j` hour template, honoring macOS's **24-hour time** switch).
The initramfs points `/etc/localtime` at the zone (`timedatectl`, Steam's Time zone setting via
`steamos-set-timezone`, and Steam's clock). `/etc/steamac/mac-timezone` remembers the last applied
zone; a different zone chosen inside SteamOS stays (until it is the Mac's zone again).
Before Steam starts, `/usr/lib/steamac/mac-clock-format` updates `b24HourClock` in the account's
`userdata/<account>/config/localconfig.vdf`, at
`UserLocalConfigStore/Software/Valve/Steam/FriendsUI/FriendsUIJSON`. This is Steam's
Settings → Time and date → 24-hour clock toggle, not a client-wide setting: on a new disk the file
does not exist before sign-in, so it is applied on the first Steam start **after sign-in**.
Desktop Mode gets `[Formats] LC_TIME` in `~/.config/plasma-localerc` (`en_GB.UTF-8` for 24-hour,
`en_US.UTF-8` for 12-hour; this also selects the time/date locale). Last applied values and
independent user overrides are kept in `~/.config/steamac/mac-clock24.json`: changing the format
in Steam or Plasma stops following the Mac for that setting, without changing the other.
With the launcher setting off, no zone or format is touched. A test can explicitly supply
`--cmdline "console=hvc0 rootwait steamac.clock24=0"` (or `1`) without changing macOS settings.

Boot and shutdown progress does not disappear before `ready`: the first click or keypress in the
window collapses the full-screen overlay into a progress pill at the bottom center (stage,
percentage, bar, detail line such as `378 / 564 MB · 1.9 MB/s`; input reaches the guest). Clicking
the pill or View → Show Boot Progress expands it again; with the overlay disabled (Settings >
General), the pill appears immediately. While a new disk is being prepared or the Steam client is
being downloaded / installed, clicks and keypresses do not collapse the overlay, and an overlay
previously collapsed by clicking expands automatically (`overlay: expanded for
steam-download`); one collapsed via View → Show Boot Progress remains a pill until `ready`. Before
`ready`, the window title repeats the stage: “FX Steam Launcher — Downloading Steam update 70%”,
“— Starting Steam…”, “— Shutting down…”. The log records `overlay: collapsed to pill (click)` /
`expanded from pill`. After `ready`, if the window has had no picture for ≥ 3 s (scanout is off, or
no frame has arrived after scanout was set or resized), or the displayed frame has been black for
≥ 5 s (sparse sampling of 64 × 40 points, brightness < 8/255 for ≥ 99.5%, no more than 4 times a
second, ~3 µs), and the focus is not in a game and the guest is not asleep / paused / suspended,
the pill says “Waiting for SteamOS to draw…” with the reason, agent heartbeat, and VM CPU usage;
it disappears on the first non-black frame (`no-picture: shown after 5.0 s (black picture …)` /
`hidden after … (first non-black frame)`). Test:
`work/out/steamac-vm --selftest-pill --selftest-out DIR`.

While a game has focus (`focus game <appid>`), if the guest sends no GPU commands for 2 s (the
virtio-gpu control queue and Venus rings — `krun_gpu_get_activity` counters), a card saying “Still
working — loading or compiling shaders…” appears over the last frame with VM CPU usage; if the
guest agent has also stopped sending heartbeats (> 5 s), it says “SteamOS is not responding…”
(this applies regardless of focus). An idle Steam interface sends no GPU commands for minutes and
does not trigger the indicator. It disappears on the very next GPU command or when focus leaves the
game; every occurrence is logged (`stall: gpu idle 3.1 s (guest alive, …)`). With `--perf-stats`, a line
`perf: gpu ctrl/s=… ring/s=… longest-idle=…` is added every 5 s. Disable it in Settings > General. The agent runs in each
gamescope session (gaming and Desktop Mode): when a session ends (Switch to Desktop, Return to Gaming
Mode), no heartbeat is expected until the new session's agent sends one; while the VM is suspended or
asleep the indicator is off, and after resume, guest wake, and Mac sleep, idle and heartbeat timers
start over.

| Keys in the window | |
|---|---|
| Ctrl+Cmd+F | full screen (macOS turns on Game Mode: Info.plist declares a game — `LSApplicationCategoryType` `public.app-category.games`, `GCSupportsGameMode`, `LSSupportsGameMode`; `gamepolicyd` logs “Game mode status is now on”) |
| Ctrl+Cmd+G | manually capture / release the mouse |
| Ctrl+Cmd+P | toggle Apple's Metal Performance HUD (FPS, frame interval, GPU time, memory); also View → Show Metal Performance HUD and Settings > Display |
| Ctrl+Option | release the captured mouse |
| close window | shut down the guest (power button) |

Mouse (`--mouse auto`, by default): the SteamOS cursor follows the Mac cursor precisely. gamescope
(gaming mode) does not accept absolute coordinates, so the launcher moves it with relative deltas
without acceleration. When a game has focus in the guest (the agent sends `focus game <appid>`), the
first click captures the mouse (relative movement for mouse-look); Ctrl+Option releases it. On
returning to Steam, capture is released automatically. **Mouse** menu:
- **Capture Mouse in This Game** — auto-capture for the current game (saved by appid);
- **Auto-Capture Mouse in Games** — default for all games;
- **Capture / Release Mouse Now** — same as Ctrl+Cmd+G.

These and all other settings are in the **Settings** window (see below), domain
`es.fxgam.steamac` (`defaults read es.fxgam.steamac`); on first launch they are copied once from
the previous domain `dev.steamac.vm`. Metal stores the shader cache by app identifier, so after
changing the identifier, the first launch of games compiles shaders again (one “cold” start).
`--auto-capture on|off` overrides the default for one launch.
`--mouse tablet` — absolute tablet (gamescope ignores it, so only for other guest compositors),
`--mouse capture` — always capture on click.

Guest access: `ssh -p 2222 steamos@127.0.0.1`, password `steamos` (change via
`STEAMOS_PASSWORD=... scripts/build-image.sh disk`). The hvc0 console is in the terminal running
`run.sh`.

SSH is toggled with one switch: **Settings → Advanced → Enable SSH** (or `--ssh-port N` /
`--no-ssh`). On each boot the launcher passes `steamac.ssh=0|1`: with 0, the Mac port is not opened
at all (gvproxy without forwarding), and initramfs masks sshd in SteamOS. By default SSH is enabled
in the dev launcher (`work/out/steamac-vm`, `./run.sh`, port 2222) and disabled in
`FX Steam Launcher.app` (the `SteamacReleaseDefaults` key in Info.plist, set by `bundle.sh`). When
enabled, the launcher generates a password for user `steamos` (20 characters, SecRandomCopyBytes),
stores it in the Keychain separately for each disk (by its GPT GUID), and on the next boot passes
only its SHA-512 crypt hash to the guest (“config payload” disk, `steamac.config=1`; the guest
responds `config applied`). Settings shows the user, password (Show/Copy), ready-to-use command
`ssh -p … steamos@127.0.0.1`, status “applied / will apply on next start”, and **Regenerate Password**;
in the dev launcher, the password is generated only via the button (Docker-built disks keep
`steamos`). Disks created in the app receive a password only this way — they have no default
password. From the terminal, `steamac-vm --ssh-password <disk>` prints the user, password, and
status.

## LAN networking and Steam Remote Play

Ordinary networking uses gvproxy user-mode NAT (`192.168.127.2` in SteamOS); LAN broadcasts
do not cross that NAT. **Settings → Advanced → LAN Remote Play** (next start), or
`--lan-remote-play`, enables a launcher-side discovery relay and same-port forwards:
UDP **27031–27036**, TCP **27036–27037**, from the Mac to the guest. It is **off by default**:
enabling it exposes Steam's Remote Play services to other machines, independently of SSH.
`--no-lan-remote-play` disables it for one boot; Network off disables it too.

Allow macOS's **Local Network** permission and incoming traffic in the Mac/SteamOS firewall.
Steam Link and the Mac must be on the **same IPv4 subnet** (Wi-Fi client isolation, guest
networks, VLANs, routed discovery and IPv6-only LANs are not supported). Enable Remote Play
in guest Steam; use the Mac's LAN address for manual pairing, not `192.168.127.2`.
Quit the Mac's own Steam client if it owns these ports: the launcher never shares or steals
UDP 27036, logs conflicts as `remote-play: disabled for this boot`, and rolls back its forwards.
The Mac Steam client's own UDP 27036 broadcasts are not relayed back into the guest.

The relay preserves Steam client identity and unknown protobuf fields, replaces status
address hints with the Mac's LAN IPv4 address, and retains ports because the forwards use
the same port numbers. Guest status announcements are refreshed by real discovery queries
every five seconds (gvproxy does not export unsolicited guest-subnet broadcasts); no status
is invented when Steam does not answer. Steam may not advertise a signed-out host.
`--selftest-remote-play` checks packet parsing and address rewriting without booting a VM.
For a real LAN probe (on the Mac or a second machine), run
`python3 scripts/test/remote-play-discovery.py --bind <LAN-IP> --broadcast <subnet-broadcast> --expect-host <Mac-LAN-IP>`.
It uses an ephemeral port, prints the actual Steam reply and rewritten address, and fails if
no host answers; it does not claim pairing or streaming success.
Protocol references: [Valve's Remote Play network settings](https://help.steampowered.com/en/faqs/view/3E3D-BE6B-787D-A5D2),
[Steam remote-client protobufs](https://github.com/SteamDatabase/Protobufs/blob/master/steam/steammessages_remoteclient_discovery.proto),
and [discovery envelope framing](https://github.com/OpenSourceLAN/steam-discover/blob/master/listener.js).

