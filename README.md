# Days Gone LAN ghost co-op prototype

An experimental Windows x64 ASI plugin that displays one LAN partner in your
Days Gone world. **This is not full campaign or combat co-op.** Each game still
runs its own single-player simulation. Use disposable saves for testing.

## What it does

- Exchanges player position, heading and velocity over UDP at approximately 30 Hz.
- Spawns a separate, non-colliding skeletal avatar for the partner, using local
  Deacon meshes, modular parts and materials.
- Smooths movement on a sender-time timeline, with adaptive 80-250 ms buffering,
  monotone cubic interpolation and up to 150 ms of bounded extrapolation.
  Timestamps follow the engine's frame-start clock, not the HUD draw time.
- Plays idle/walk/run/sprint animations according to received speed.
- Hides the avatar after five seconds without state updates.
- Replicates motorcycle position/orientation and rider animations, with a visual
  clone of the observing player's motorcycle.
- Offers experimental passenger riding on the partner's bike.
- Replicates seven jacket/trouser/cap color presets and cap visibility.
- Includes an in-game settings overlay with keyboard, mouse and gamepad controls.
- Includes experimental, mutually opted-in friendly fire; see its validation
  status and limitations below before relying on it.
- **F7** teleports your player near the partner while the game window is focused.
  This is experimental: there is no terrain, streaming or safe-landing check.

The avatar is a visual ghost, not a second AI/combat pawn. Enemies, inventory,
loot, missions, doors, time, weather and saves are **not synchronized**. Detailed
aiming, crouching, weapon/projectile effects, motorcycle upgrades, fuel, damage
and physics are not replicated. Equipment is cloned locally; only the selected
color presets and cap visibility describe the partner's appearance.

Only two players are supported. The protocol has no authentication or encryption:
use a trusted LAN, never Internet port forwarding. The first valid endpoint in
host mode is pinned until the game restarts. Restart both games if a peer changes
IP or port. A timeout allows sequence numbering to restart, not a different endpoint.

## Supported game

- Steam app **1259420**, build **19221447**.
- DaysGone.exe SHA-256:
  `17c2fca01d566914147a6d4924d038cb1f5a822c2829e4c45862e2bf837730b3`.
- Engine metadata: Unreal Engine 4.10.1, with Bend-specific layouts.
- Tested runtimes: CachyOS on `cachy` and SteamOS on a Steam Deck, using Proton.
- Two real games have been tested together. Native Windows remains untested.

The runtime checks the PE timestamp, image size and ProcessEvent prologue before
installing a build-specific hook. These checks are not a full executable hash
check. Do not disable them, update offsets speculatively or assume compatibility
with another game release or another gameplay hook.

## In-game controls and settings

| Action | Keyboard/mouse | Gamepad |
|---|---|---|
| Open/close settings | F8 | View/Back + Y |
| Select a row | Up/Down or click | D-pad Up/Down |
| Change/activate | Left/Right, Enter or click | D-pad Left/Right or A |
| Back/close | Esc | B |
| Join/leave partner's motorcycle | F9 or menu | View/Back + X or menu |
| Teleport near partner | F7 or menu | Menu |

The game must be focused. The overlay does not pause either world. Colors,
cap visibility, motorcycle synchronization and passenger permission save to the
installed INI immediately. The palettes are Original, Crimson, Ocean, Forest,
Sand, Charcoal and Violet. Hat customization currently means the existing cap's
color and visibility, not alternative hat models.

For a passenger, both players enable motorcycle sync; the driver allows a
passenger, mounts their own bike normally and stops nearby. The passenger joins
while on foot. This is a kinematic attachment to the visual bike, **not native
two-seat vehicle simulation**. It temporarily disables passenger movement and
collision, hides the native body in favor of an independent posed body, and
blocks saving. Passenger weapon attacks are not supported. Leaving restores
captured native visibility/state and checks
for safe ground/capsule clearance; an unsafe manual exit is refused.

Friendly fire defaults to **OFF every launch** and is deliberately not persisted.
Both players must enable it and confirm lethal damage in their respective menus.
Turning either side off blocks further accepted attacks. The transport scopes
damage to the current connection and consent epochs, retransmits until
acknowledged, deduplicates and expires old attacks. This does not authenticate
the peer: trusted LAN only. Damage currently uses the midpoint of the weapon's
configured native range; full vanilla critical/skill/falloff parity is not claimed.
Real melee and firearm hits have reduced the other game's native health.
The two-player test also confirmed death and normal reload after lethal damage.
This does not establish full vanilla damage parity.

Both clients must run protocol **v2 (128 bytes)**; v1 clients are incompatible.
When upgrading an existing installation, replace the ASI but preserve each
machine's configured INI. Keep the supplied Dear ImGui license with distributions.

## Install

1. Close the game and back up your saves. Launching/loading/teleporting can trigger
   ordinary game autosaves; the plugin does not isolate or roll back saves.
2. Install the x64 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   next to the game executable if you do not already have a compatible loader.
   Do not overwrite an existing loader or unrelated mod.
3. Copy **both** [DaysGoneCoop.asi](package/DaysGoneCoop.asi) and
   [DaysGoneCoop.ini](package/DaysGoneCoop.ini) into:

   ```text
   Days Gone/BendGame/Binaries/Win64/
   ```

4. On Proton, a loader named `version.dll` needs a native DLL override. If you
   configure it via Steam launch options, preserve any existing options:

   ```sh
   WINEDLLOVERRIDES="version=n,b" %command% --launcher-skip
   ```

   If other overrides exist, merge them instead of replacing them. `cachy`
   already has an app-specific `version=native,builtin` override in the game's
   Proton prefix, so it does not need an additional Steam launch-option change.
5. Configure host/join below, allow inbound **UDP 27777** on the LAN firewall,
   and start the game on both machines. Each player must load their own save.

For Proton, the usual game location is beneath
`~/.local/share/Steam/steamapps/common/`. Saves are beneath
`steamapps/compatdata/1259420/pfx/drive_c/users/steamuser/AppData/Local/BendGame/Saved/`.
An earlier development session backed up `cachy`'s saves to
`~/DaysGoneCoop-save-backup-20261017-223310`.

## Host and join

On **cachy (host)**, edit the installed INI:

```ini
[Coop]
Port=27777
PeerHost=
PeerPort=27777
PlayerName=Host
```

On the **second PC**:

```ini
[Coop]
Port=27777
PeerHost=cachy
PeerPort=27777
PlayerName=Partner
```

Use `cachy`'s IPv4 LAN address if hostname resolution fails. Each player has an
independent world; load saves in the same region and travel close together.
The host learns the joiner's source endpoint from the first valid packet.
Configuration changes require restarting the game.

On the tested LAN, `cachy` is `192.168.1.131`; hostname resolution was intermittent,
so the IP address was used for the LAN test. With approval, this narrow UFW rule
was added on `cachy` (the firewall remains enabled):

```sh
sudo ufw allow in on eno1 from 192.168.1.0/24 to any port 27777 proto udp comment "Days Gone co-op LAN"
```

To remove that rule later:

```sh
sudo ufw delete allow in on eno1 from 192.168.1.0/24 to any port 27777 proto udp
```

The other settings are documented in [the template](config/DaysGoneCoop.ini).
`TeleportKey=118` means F7 (decimal Windows virtual-key code). `Discovery=1`
writes component details and a list of loaded animations for investigation.
Animation names refer to already loaded assets; the mod does not ship game assets.

### Steam Deck joining cachy

The Steam Deck installation uses the same PC plugin under Proton, including the
`version.dll` loader and its native DLL override. The installed Deck INI has
`PeerHost=192.168.1.131`, `PeerPort=27777` and `PlayerName=SteamDeck`; cachy uses
host mode with `PlayerName=cachy`. Launch Days Gone normally through Steam on
both devices and load saves in the same area. Do not run the simulated-partner
start scripts for this two-device test.

The menu reads XInput, not native Steam Input actions. The official Days Gone
layout did not expose an XInput controller on the tested Deck, so its mod-menu
chords did nothing. The Deck was switched, with approval, to Valve's **Gamepad
With Joystick Trackpad** template under Controller Settings > Current Layout >
Templates. The running game then reported XInput mask `0x111` (slot zero through
each of the three providers), rather than `0x0`. This replaces the official
action layout, including its rear-grip assignments; restore it from Recommended
if desired. The user confirmed that the menu and navigation work and normal
movement/driving still work after closing it. No native-map interference was
reported in that test.

Teleport is available in the mod menu. F7 remains an optional Steam Input
keyboard binding.

## Diagnostics and removal

The plugin writes `DaysGoneCoop.log` next to itself. Check for:

- `ProcessEvent hook installed`
- `GNames ...` and engine class resolution
- `network: listening ...`
- `peer ... connected`
- `avatar: spawned ...`
- Periodic partner and avatar coordinates, with the avatar's screen position
  (`screen=(-1 -1)` means behind the camera) and component visibility
- `perf:` lines with mod updates/s, mean/peak mod Tick time, send/receive rates,
  riding-animation lookup time, adaptive delay and cumulative buffer counters.
  Tick time is plugin game-thread work, not the whole game's frame time.

A `FATAL` entry means the plugin disabled its gameplay work; do not bypass the
check. No partner usually means mismatched INI settings, name resolution, the
firewall, or one player not having loaded a save. No avatar with a connected
peer can mean missing meshes or different world positions. Consult the log.

To uninstall, close the game and remove only `DaysGoneCoop.asi` and its INI.
Optional plugin logs and animation lists can also be removed. Keep shared
loaders and other mods. Existing diagnostic probe source in this workspace is
historical and is not built by the current build script.

## Build and tests

Requires MinGW-w64's `x86_64-w64-mingw32-g++` for the plugin, a native C++20 compiler
for protocol tests, and Poetry with Python 3.11+ for tooling tests.

```sh
poetry install
make test
make build
```

The build emits a statically linked Windows x64 DLL at
`package/DaysGoneCoop.asi` and copies the INI template alongside it. Building
does not install anything or touch Steam, saves or remote machines.

Tests cover the actual C++ packet decoder, consent/retransmission, sequence and
timestamp wraparound, and motion under jitter, batching and packet loss, plus
Python protocol/handshake tests and offline PE inspection. They do not prove
two-player gameplay, game-thread safety or stability through every world transition.

### Single-game simulated partner

To try the mod on one PC, without a second player, copy `tools/fake_peer.py`,
`tools/same-pc/start-test.sh` and `tools/same-pc/stop-test.sh` into one folder
on the gaming PC (Linux/Proton with systemd), then run:

```sh
./start-test.sh            # follow mode (default); also: circle | still
```

It backs up the INI, points it at `127.0.0.1:27778`, starts the simulator as
the `dg-coop-fakepeer` user service (log: `fake_peer.log` next to the scripts)
and launches the game. Load a save. A second Deacon appears in front of you
and to the right, then walks, runs or sprints to keep up. Loading a save or
fast travelling rebuilds him with your current outfit after about five seconds.
F7 still teleports you to him. Running the script again switches mode while the
game keeps running. `./stop-test.sh` stops the simulator and restores the
host-mode INI; restart the game afterwards.

To run the simulator by hand against a game in host mode:

```sh
poetry run python tools/fake_peer.py --game cachy:27777 \
  --port 27778 --offset 300 --radius 150 --speed 150 --duration 60
```

The helper sends a handshake, receives the game's position and moves a ghost
around it. Units are centimetres. Use `--speed 0` for a stationary partner, or
`--follow` (with `--speed` as the top speed) to trail the player. Stop with
Ctrl-C, or use `--duration` for a bounded run. Restart the game to switch from
this test endpoint to a real partner.

### Offline executable investigation

```sh
make inspect EXE="/path/to/DaysGone.exe" \
  MANIFEST="/path/to/appmanifest_1259420.acf" > engine-inspection.json
```

This read-only tool verifies the full fingerprint against
[the build inventory](config/steam-19221447.json), then reports static analysis
candidates. Those candidates are not themselves callable engine addresses.
See [PE inspection details](docs/PE-INSPECTION.md). For how the mod was
reverse engineered and how it hooks the engine, see
[the technical write-up](docs/TECHNICAL.md).

## Validation status

Two real game instances on `cachy` and a Steam Deck were tested over the LAN on
2026-10-18. Both loaded the plugin and a progressed save, connected to each
other, and spawned modular partner avatars. Moving the Deck changed the remote
avatar's position on cachy; moving cachy was received on the Deck, including a
walk animation in its log. The Deck-controlled partner was visually confirmed
on cachy with textures. F7 teleport was exercised on the Deck using keyboard
input. This is a real-game-to-real-game movement test, not the simulator.

Expansion tests verified the settings overlay, visible garment tinting, cap
hiding, native bike mounting, modular bike proxies, passenger boarding/following
and safe dismount. Separate Original/Crimson and Crimson/Ocean appearances have
been visually confirmed on the two clients. Restoring Original after several
minutes was also checked. In the earlier matching build on both devices, the
passenger's component-space hips stayed at 64.2 cm and the stationary visual
root varied by about 0.1 cm after settling. Short rolling/braking, manual
dismount/reboarding, driver dismount and driver-disconnect cleanup were checked.
The player subsequently reported jitter while moving. The revised networking
uses sender timestamps rather than packet-drain times, smooths bike motion as
well as player motion, and anchors the driver's view of the passenger directly
to the local native seat instead of a round-trip-delayed position. A new short
drive/brake check showed the seated passenger on both clients. The subsequent
physical playtest reported **"much smoother, not perfect"**: residual moving
jitter remains, so this is not a claim of fully solved passenger motion.
A follow-up build fixes one measured source of that jitter. HUD-draw time varies
within a frame, so timestamps now use a frame-locked engine clock. On both
clients, the seated passenger stayed visible through a drive/brake check, and
the clock remained locked. Physical smoothness has not yet been retested.

Profiling found roughly 9.5-9.9 ms mean / 122-129 ms peak mod Tick time on cachy,
with sends falling to about 18/s. After caching repeated object discovery,
following HUD-frame cadence, correcting send deadlines and backing off absent
controller polling, the final riding check measured about 0.67-0.73 ms mean /
5.6-7.2 ms peak on cachy and 1.77-1.96 ms mean / 18.9-23.7 ms peak on the Deck,
with roughly 30 sends/receives per second on both. Animation lookups dropped from about
1.9 ms per call to below the log's 0.01 ms resolution. These are measurements of
mod work, **not whole-game FPS benchmarks**.

`make test` passes 35 Python tests plus native C++ protocol/consent/retransmission
tests; `make build` cross-compiles the Windows plugin with warnings as errors.
Real knife hits reduced the other game's native health (121.9 to 106.9 for one
20-point attack before native processing). A one-sided opt-in test observed a
native melee collision window without queuing damage while the partner was off.
Pistol damage was confirmed by the user and native health logs, including
121.9 -> 92.6 -> 63.4 -> 34.1 -> 4.9 -> -24.4; the user confirmed death/reload.
Wire health clamps native negative death values to zero. Physical Deck menu
opening/navigation and normal gameplay controls were confirmed after switching
layouts. Residual moving jitter, long-distance travel, native Windows and
long-session stability still need work or further validation. Full
shared-world co-op still requires game-specific replication and authority work.

Do not commit credentials, the `sshpw` file, game executables, assets, saves or
runtime logs.
