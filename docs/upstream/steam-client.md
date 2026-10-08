# Steam client

Which Steam client SteamOS launches is selected in the launcher: the first-launch window, the
**Create SteamOS Disk** window, and **Settings → Advanced → Steam client** (applies on next start,
“Restart VM to apply”); for a single launch, use `--steam-client frame|deck|deckbeta`. The launcher
passes the selection on every boot in the kernel cmdline as `steamac.steam_client=…`;
`/usr/lib/steamac/steam-client` in the layer reads it each time Steam starts. The Steam service
remains the stock SteamOS service (`steam.service`); the layer adds only a
`steam.service.d/50-steamac.conf` drop-in to it. Before startup, `steam-client` copies the stock
`/usr/share/deckard/RUNSTEAM.sh` to `~/.local/share/Steam/` — unchanged for the Frame client with
a remembered account, while in `deck`/branch/sign-in modes it removes only the lines containing
the `-deckard` and `-vrgamepadui` arguments from the copy. Valve's files are not included in the
layer.

| Option | What it is | Pros and cons |
|---|---|---|
| **Steam Deck client** (`deck`, by default) | public ARM64 Steam Deck client, `steamdeck_stable` branch (the same build as the public `steam_client_linuxarm64`; not officially announced for ARM) | normal sign-in with an on-screen QR code; a public client branch rather than an internal beta |
| **Steam Deck client (beta)** (`deckbeta`) | `steamdeck_publicbeta` branch | like `deck`, but beta |
| **Steam Frame client** (`frame`) | Valve's beta client for Steam Frame (`linux_arm64_beta_<hash>`, flags `-deckard -vrgamepadui`) — as in the image | the client the image ships; an internal beta for a device not yet released; signing in uses sign-in mode (below) |

Changing the option makes Steam download a different client on the next start (up to ~1 GB,
progress in the boot overlay); when switching back to Frame, the Steam bootstrapper switches
automatically based on the `-deckard` flag. A manual `/etc/steamac/steam-client-branch` inside
SteamOS (any client branch) still works when `frame` is selected (or when the launcher passes no
parameter — older versions, a custom `--cmdline`); the launcher's `deck` / `deckbeta` selection
takes precedence over the file.

