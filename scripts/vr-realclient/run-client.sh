#!/usr/bin/env bash
# Launch the locally built Linux WiVRn client in an existing graphical session.
# Usage: run-client.sh [client-binary]
set -euo pipefail

BINARY=${1:-"$HOME/vr-realclient/build-client/bin/wivrn"}
LOG_DIR=${VR_REALCLIENT_LOG_DIR:-"$HOME/vr-realclient/logs"}
mkdir -p "$LOG_DIR"

if [[ ! -x "$BINARY" ]]; then
    printf 'WiVRn client executable not found or not executable: %s\n' "$BINARY" >&2
    exit 2
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'No active X11 or Wayland display. Run from the x86 host graphical session; this helper does not create a virtual display or start Monado.\n' >&2
    exit 2
fi
if [[ -n "${XR_RUNTIME_JSON:-}" && ! -r "$XR_RUNTIME_JSON" ]]; then
    printf 'XR_RUNTIME_JSON is set but unreadable: %s\n' "$XR_RUNTIME_JSON" >&2
    exit 2
fi

LOG="$LOG_DIR/client-run-$(date -u +%Y%m%dT%H%M%SZ).log"
printf 'Launching %s\nLog: %s\n' "$BINARY" "$LOG"
# Keep the GUI attached to this session; select the Mac host in the WiVRn UI.
"$BINARY" 2>&1 | tee "$LOG"
