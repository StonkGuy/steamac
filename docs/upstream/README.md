# steamac documentation (upstream)

This is the documentation of upstream [steamac](https://github.com/fxgl/steamac), the
ARM64 SteamOS (Steam Frame image) VM for Apple Silicon. It is licensed under the Apache
License 2.0, © 2026 FX GAMES FZ LLC (see the repository's `LICENSE` and `NOTICE`).

The upstream `README.md` was split into these pages by this fork; the text is upstream's,
only reorganised (the section headings became the pages below). For this fork's own
documentation, start at the repository's top-level `README.md` and `docs/fork.md`.

- [Overview](overview.md) — what the project is and how it runs
- [Requirements and building](requirements-and-building.md) — requirements, build/run, LAN networking and Steam Remote Play
- [Settings window](settings.md) — the Settings tabs, VM memory, growing the disk
- [Suspend](suspend.md) — suspending and resuming the VM
- [SteamOS sleep](sleep.md) — Sleep in SteamOS and how the launcher handles it
- [Desktop Mode](desktop-mode.md) — KDE Plasma nested in gamescope
- [Clipboard](clipboard.md) — sharing the clipboard with SteamOS
- [Controller](controller.md) — the virtual gamepad, rumble, DualSense passthrough
- [FX Steam Launcher.app](app-bundle.md) — the app bundle, disk locking, fsck
- [Creating a disk](creating-a-disk.md) — creating the SteamOS disk without Docker
- [Steam client](steam-client.md) — Deck / Deck beta / Frame client selection
- [Vulkan driver](vulkan-driver.md) — KosmicKrisp and MoltenVK
- [Signing in](signing-in.md) — signing in to Steam in the VM
- [Distribution (DMG)](distribution.md) — building and notarizing the DMG
- [Crash reports](crash-reports.md) — Sentry crash reports and diagnostics
- [Update check](update-check.md) — the launcher's update check
- [Report a Problem](report-a-problem.md) — the in-app report dialog
- [How it works](how-it-works.md) — repository layout and notable fixes
- [Status](status.md) — what has been verified
- [Limitations](limitations.md) — known limitations
- [License](license.md) — licensing of the upstream project

The upstream [Русский](README.ru.md) README is also included here, unchanged.
