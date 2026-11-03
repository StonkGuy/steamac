# Game statistics

Which games start and how fast they run on which Mac, collected from players who agree and shown
to everyone at <https://fxgl.github.io/steamac/games.html> (`docs/games.html`).

The guest agent watches Steam's game scopes (`app-steam-app<appid>-*.scope`, the same cgroups
**Pause the game** freezes) on its 1 s heartbeat and reports `game-start <appid>`, `game-mem <appid>
<anon_kib> <guest_used_kib>` every 5 s (the game's anonymous memory and the guest's used memory),
`game-proton <appid> <version>` (the prefix's `compatdata/<appid>/version`), immediately followed by
`game-gfx <appid> dxmt|vulkan` (the actual graphics path), and `game-exit <appid>`;
`game <appid> <name>` follows the first `game-start` or focus of an app id per boot. The launcher
(`GameStats.swift`) makes one report per game run: how long it ran and had focus (not paused, VM
not suspended), whether a window ever appeared and after how long (a run without one is a failed
start), frame rate from the guest's scanout-0 flush intervals while the game has focus (average,
median, 1% low, hitches over 50 ms per minute; gaps over 1 s are left out; libkrun's
`present_frame`, the same flushes `--perf-stats` reports), peak game / guest memory, the VM
process's peak `phys_footprint` on the Mac, resolution, Proton version, and the end (`exit`,
`shutdown`, `session`). Each report carries the Mac (model, chip, P and total cores, GPU cores,
RAM, macOS), the VM (vCPUs, RAM, Vulkan driver, wmtr, refresh rate, SteamOS build) and the
launcher version.
New session and rating reports use schema `v:2`, with optional `gfx` (`vulkan` or `dxmt`) and `env`
(the allowlisted launch words described in “Per-game profiles”, at most 256 characters). The actual
graphics path comes from `game-gfx`; ratings use the last session's graphics path and launch words.
The service still accepts v1 (including previously saved sessions), storing its `gfx`/`env` as NULL.
Invalid v2 assignments are rejected with HTTP 400; valid ones are sorted by name, last duplicate wins.

Every report is kept on this Mac (`~/Library/Application Support/es.fxgam.steamac/game-stats.json`,
20 sessions per game) and listed in **Settings → Games**. Nothing leaves the Mac until **Share
game statistics** is on: after the first game with a window the launcher asks once, in a window
beside the VM window that does not take the keyboard or the captured mouse (**Share** / **Don't
Share**); the switch in Settings → Games changes it at any time. While it is on, finished sessions
and ratings go to the statistics service (`POST /v1/sessions`, `POST /v1/ratings`), sessions
recorded earlier included; a game whose **Share** box is off sends nothing and its rating is
withdrawn (`"rating": "none"`). Uploads go one at a time; HTTP 400/413 drops a report, anything
else is retried after 10 minutes, at the next session end or the next boot.

Privacy: no name, user or computer name, Steam account, files or arbitrary typed text (only the
allowlisted graphics launch words above are shared); the install ID is a
random UUID used only for distinct-Mac counts and rate limits (not the crash reports' ID); the
service uses the client address only as a transient rate-limit key and never stores it.

The page publishes aggregates only, never single sessions, and lists the chips each game was tested
on. Configurations group by chip, RAM, Vulkan driver, graphics path and launch words (`gfx`/`env`;
`wmtr` is retained for compatibility). For old rows, NULL `gfx` means `vulkan` when `wmtr=0`,
but unknown when `wmtr=1`: permission to use DXMT does not prove a game used it.
Its **My Mac** filter keeps only reports from Macs that are not better than the visitor's (GPU cores,
RAM and performance cores each at most the given value; the API's `max_gpu_cores`, `max_ram_gb`,
`max_pcores`): what runs there should run here. **Open Statistics Page** in the launcher passes this
Mac's values (`?chip=…&gpu=…&ram=…&pcores=…`) and turns the filter on; the page remembers them.

`GET /v1/games/<appid>` also returns `recommended` (or `null` if no profile qualifies).
It groups `(driver, gfx, env)` across all chips within the same My Mac filters, requiring at least
two distinct installations across sessions or ratings; unknown graphics paths cannot win.
Score = `(well + 0.5 × issues) / total ratings` (zero without ratings); ties favor higher
`launch_rate`, then lower `early_exit_rate`, with missing rates last and deterministic
driver/graphics/env ordering after that. The result includes driver, graphics path, launch words,
install/session counts, ratings, median average FPS and launch rate. The site and launcher show
the recommendation, but it is never applied automatically.

Service: `services/game-stats` — a Cloudflare Worker with a D1 database (strict validation,
idempotent session IDs, 30 POSTs per minute per address, a daily cap per install, aggregates
`GET /v1/games` and `GET /v1/games/<appid>` cached for 5 minutes in D1, because the Cache API does
nothing on `workers.dev`). Deploy once:

```sh
cd services/game-stats && npm install
npx wrangler login
npx wrangler d1 create steamac-game-stats      # put the printed database_id into wrangler.toml
npx wrangler d1 migrations apply steamac-game-stats --remote
npx wrangler deploy                            # prints https://steamac-game-stats.<account>.workers.dev
```
For an existing deployment, migration `0002_profiles.sql` adds `gfx`/`env` to sessions and ratings
and updates the grouping indexes while preserving v1 data; `0003_responses.sql` adds the response
cache table. Run `npx wrangler d1 migrations apply steamac-game-stats --remote` **before**
`npx wrangler deploy`.

The Worker URL goes into `host/launcher/Info.plist` (`SteamacGameStatsURL`) and `docs/assets/games.js`
(`API`). Local: `npx wrangler d1 migrations apply steamac-game-stats --local && npx wrangler dev`,
`npm test`.

Tests: `STEAMAC_GAME_STATS_URL` overrides the service (and enables uploads in the dev launcher
`work/out/steamac-vm`, which otherwise sends nothing), `STEAMAC_GAME_STATS_FILE` the local file;
FIFO `--control-fifo`: `guest game-start <appid>` etc. feed the tracker, `stats state|flush|ask|dump
PNG|reset-question`, `stats press share|decline`, `settings games`.
