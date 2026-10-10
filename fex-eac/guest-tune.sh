#!/bin/sh
# Guest-side tuning for EAC games. Idempotent; needs sudo.
set -eu

# MangoApp is a gamescope performance overlay layer: with it loaded, gamescope composites the game into
# a layer instead of scanning it out directly, which costs frames. Write the session conf gamescope reads.
CONF=/etc/steamac/gamescope-session.conf
sudo mkdir -p "$(dirname "$CONF")"
if sudo test -f "$CONF"; then
  # Keep every other setting; drop the old MangoApp line if one is already there.
  sudo sed -i '/^STEAMAC_GAMESCOPE_MANGOAPP=/d' "$CONF"
fi
echo 'STEAMAC_GAMESCOPE_MANGOAPP=0' | sudo tee -a "$CONF" >/dev/null
echo "wrote $CONF:"
sudo cat "$CONF"

cat <<'EOF'

Launch options for VRChat (Steam > VRChat > Properties > Launch Options; $HOME is the guest home):
  env EAC_LAUNCHERDIR=$HOME/.local/share/Steam/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat PROTON_EAC_RUNTIME="$HOME/.local/share/Steam/steamapps/common/Proton EasyAntiCheat Runtime" WINEDEBUG=-all PROTON_USE_XALIA=0 WINE_CPU_TOPOLOGY=16:0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3 %command%

The topology string tells the game it has 16 CPUs mapped onto the guest's 4 vCPUs: IL2CPP sizes its
thread pool from that number, and without it the pre-join region lookup starves and stalls. Keep the
16 but list the guest's actual vCPU ids (nproc, and the ids gamescope leaves for the game), not the
0-3 repeated above.
EOF
