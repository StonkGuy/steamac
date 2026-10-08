# Distribution (DMG)

`host/launcher/dist.sh` turns the built `work/out/FX Steam Launcher.app` into the downloadable
`work/out/dist/FX-Steam-Launcher-<version>.dmg` (the app + a link to `/Applications`). A copy of the
bundle without the `SteamacBuildOut` key (the path to this build tree) is re-signed with Developer ID,
hardened runtime, and a secure timestamp: first all nested Mach-O files (`Frameworks/*.dylib`, helper
programs in `Resources`), then the bundle with `steamac-vm.entitlements` (hypervisor,
disable-library-validation, and audio-input — without the latter, hardened runtime silently blocks the
microphone). The app is notarized and stapled, then the DMG is signed, notarized, and stapled —
Gatekeeper allows it through even offline (the usual “downloaded from the Internet” prompt still
appears on first launch).

```sh
host/launcher/build.sh      # fresh bundle
host/launcher/dist.sh       # signing, notarization, DMG
```

A notarytool profile must be stored in Keychain once:
`xcrun notarytool store-credentials steamac-notary --apple-id <Apple ID> --team-id V25VKGTW55
--password <app-specific password>`. Variables: `STEAMAC_SIGN_IDENTITY` (by default, the only
“Developer ID Application” in Keychain), `NOTARY_PROFILE` (by default, `steamac-notary`);
`--no-notarize` — signing only, for local checks (Gatekeeper will reject a downloaded copy).

Licenses: `bundle.sh` puts all third-party license texts for bundled components and the
`THIRD-PARTY-NOTICES.txt` index (component, version, SPDX, location in the bundle, sources; generated
by `host/launcher/licenses.sh` in `work/out/licenses`, including libkrun crates and Go modules from
gvproxy/desync) in `Contents/Resources/licenses`, plus the project's `../../LICENSE` and `../../NOTICE` in
`licenses/steamac/`. `dist.sh` calls `scripts/gpl-sources.sh` and places
`work/out/dist/FX-Steam-Launcher-<version>-gpl-sources.tar` alongside the DMG — the complete source
code of GPL components (the kernel with patches and configuration, busybox from the Debian snapshot,
dosfstools, e2fsprogs, btrfs-progs, build scripts, `README.txt`); attach it to the GitHub release
alongside the DMG. For an older release: `scripts/gpl-sources.sh v1.2`.

