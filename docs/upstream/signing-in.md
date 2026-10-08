# Signing in

The Steam Frame client's sign-in screen is designed for a headset: “Tap to confirm” pairs with a
phone over Bluetooth LE, and “Scan QR code” opens a VR window — neither works in the VM (only the
password remains). Therefore, with the Steam Frame client, while `config/loginusers.vdf` has no
remembered account (new disk, signing out, signing in without “Remember me”), `steam-client`
starts Steam without `-deckard`/`-vrgamepadui`: the bootstrapper switches itself to the public
ARM64 Steam Deck client (`steamdeck_stable`), and sign-in shows an on-screen QR code (Steam Mobile
App → Steam Guard → scan) alongside the password form. After signing in with “Remember me”, Steam
restarts once and returns to the Steam Frame client (each client switch downloads up to ~1 GB;
progress is visible in the boot overlay). Without access to `client-update.steamstatic.com`,
sign-in mode is not enabled.

`RecvMsgClientLogOnResponse() : 'Try another CM'` lines in `connection_log.txt` on the sign-in
screen are normal: the CM server drops a connection without an account sign-in after ~60 s, and
the client reconnects.

