# Desktop Mode

Steam → Power → **Switch to Desktop** starts KDE Plasma; the desktop's **Return to Gaming Mode** icon
goes back. On the Steam Frame image this mode is a VR desktop (`plasma-session.target` wants
SteamVR, which the VM masks), so the layer replaces it: `plasma-session.target` and
`steamac-nested-desktop.service` run Plasma as one KWin window inside the same gamescope
(`/usr/lib/steamac/nested-desktop`, like Valve's `steamos-nested-desktop`), sized to the display, so
gamescope shows it 1:1. `gamescope-onready` waits for that service: when Plasma exits, the session
ends and SDDM logs back in. Plasma gets its own runtime directory and D-Bus session bus; the layer's
`steamosctl` shim (`/usr/lib/steamac/desktop-bin`, first in its PATH) sends SteamOS commands such as
Return to Gaming Mode to the outer session bus, where steamos-manager runs. The desktop's Steam
autostart (`/usr/lib/steamac/desktop-xdg/autostart/steam.desktop`) keeps the client chosen in the
launcher instead of the stock `-deckard` (Frame client), which would download the other client on
every switch. The progress agent reports `focus desktop <w>x<h>` (the Plasma window): the launcher
keeps the relative pointer and maps it onto that window as gamescope scales it, and a boot straight
into Desktop Mode (`steamos-session-select plasma-persistent`) reports `ready` when the desktop is up.

Flatpak apps (Discover) run in bubblewrap, which mounts its own procfs in a user namespace. The kernel
allows that only while some procfs in the mount namespace is fully visible, and the initramfs binds
the synthesized `/proc/cmdline` over the real one; so it also mounts an untouched procfs at
`/run/steamac/proc` (`nosuid,nodev,noexec`). Without it every Flatpak app exits with `bwrap: Can't mount
proc on /newroot/proc: Operation not permitted`.

