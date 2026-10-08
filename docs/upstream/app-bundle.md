# FX Steam Launcher.app

`host/launcher/build.sh` (and `./build.sh host`) builds `work/out/FX Steam Launcher.app` in
addition to `work/out/steamac-vm` (`host/launcher/bundle.sh`): `es.fxgam.steamac`, libraries
(libkrun, libvirglrenderer, libMoltenVK, libepoxy) in `Contents/Frameworks` via `@rpath`, and in
`Contents/Resources` — gvproxy, the `Image` kernel, `initramfs.cpio.gz`, `steamac-layer.img`,
desync, and Valve's CA for disk creation (licenses are in `Resources/licenses`); the icon:
`host/launcher/AppIcon.icon` (an Icon Composer document) is compiled by `actool` into `Assets.car`
(Liquid Glass on macOS 26, ready-made renders for macOS 15), with a fallback `AppIcon.icns`;
ad-hoc signing with hypervisor + disable-library-validation entitlements. The app can be moved to
`/Applications`.

Launching from Finder (without arguments) uses the kernel, initramfs, and layer from the bundle,
and the SteamOS disk from Settings → Advanced → Disk image. By default:
`~/Library/Application Support/es.fxgam.steamac/steamos.img`; otherwise, the repository's
`work/out/steamos.img` (next to the bundle or where it was built). If there is no disk, a
first-launch window offers **Create New Disk…** (see the next section) or **Use Existing Disk…**
(the image is used in place and never copied; `scripts/build-image.sh` also builds one). In this
mode, the guest console and launcher log are written to
`~/Library/Logs/es.fxgam.steamac/steamac-vm.log`; SIGUSR1 frame dumps go there too. `./run.sh`
and `work/out/steamac-vm` work as before (window settings also apply to them unless overridden by
flags).

If the image is on an external disk, on the first launch from Finder macOS asks “FX Steam Launcher
would like to access files on a removable volume” — allow it (the VM waits for the disk to open
until you respond). Signing is ad-hoc, so macOS may ask again after the bundle is rebuilt.

One disk image can only be used by one VM at a time: the VM process holds an exclusive lock
(`flock`) on the writable disk until it exits, and a second launcher (another copy of the app, e.g.
a source build next to `/Applications`, or `steamac-vm`) refuses to start with “SteamOS is already
running” instead of mounting the same file systems twice (that corrupts `/home` and `/var`). The lock
outlives a killed launcher while the guest is still shutting down. Launchers built before the lock do
not check it.

If `/home` still has errors at boot (a VM killed while writing, or a disk used by two launchers that
predate the lock), SteamOS repairs it instead of stopping at “Starting SteamOS services…”: the launcher
adds `fsck.repair=yes` to the kernel command line, so systemd-fsck runs e2fsck answering yes rather than
only the safe preen fixes. Files e2fsck cannot place again end up in `/home/lost+found`.

