# Days Gone LAN ghost co-op prototype

An experimental Windows x64 ASI plugin that displays one LAN partner in your
Days Gone world. **This is not full campaign or combat co-op.** Each game still
runs its own single-player simulation. Use disposable saves for testing.

## What it does

- Exchanges player position, heading and velocity over UDP at approximately 30 Hz.
- Spawns a separate, non-colliding skeletal avatar for the partner, using local
  Deacon meshes, modular parts and materials.
- Smooths movement with 100 ms interpolation and bounded extrapolation.
- Plays idle/walk/run/sprint animations according to received speed.
- Hides the avatar after five seconds without state updates.
- **F7** teleports your player near the partner while the game window is focused.
  This is experimental: there is no terrain, streaming or safe-landing check.

The avatar is a visual ghost, not a second AI/combat pawn. Enemies, damage, shots,
inventory, loot, missions, doors, time, weather and saves are **not synchronized**.
Bike models, riding animations, aiming, crouching and other detailed actions are
not replicated. Both players see their own locally cloned appearance, not the
other player's equipment. Materials may reflect local customization.

Only two players are supported. The protocol has no authentication or encryption:
use a trusted LAN, never Internet port forwarding. The first valid endpoint in
host mode is pinned until the game restarts. Restart both games if a peer changes
IP or port. A timeout allows sequence numbering to restart, not a different endpoint.

## Supported game

- Steam app **1259420**, build **19221447**.
- DaysGone.exe SHA-256:
  `17c2fca01d566914147a6d4924d038cb1f5a822c2829e4c45862e2bf837730b3`.
- Engine metadata: Unreal Engine 4.10.1, with Bend-specific layouts.
- Tested runtime: CachyOS on `cachy`, Proton Experimental.
- Native Windows and a second real game instance remain untested.

The runtime checks the PE timestamp, image size and ProcessEvent prologue before
installing a build-specific hook. These checks are not a full executable hash
check. Do not disable them, update offsets speculatively or assume compatibility
with another game release or another gameplay hook.

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

Normal Steam Deck controls remain available. To use the optional teleport
without a keyboard, map a spare button to keyboard F7 through Steam Input.
The plugin does not currently provide a controller-native teleport binding.

## Diagnostics and removal

The plugin writes `DaysGoneCoop.log` next to itself. Check for:

- `ProcessEvent hook installed`
- `GNames ...` and engine class resolution
- `network: listening ...`
- `peer ... connected`
- `avatar: spawned ...`
- Periodic partner and avatar coordinates, with the avatar's screen position
  (`screen=(-1 -1)` means behind the camera) and component visibility

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

Tests cover the actual C++ packet decoder and sequence wraparound, plus Python
protocol/handshake tests and offline PE inspection. They do not prove two-player
gameplay, game-thread safety or stability through every world transition.

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

Earlier simulator tests also verified host-mode learning and reconnection after
a timeout. Combat, long-distance travel, bike riding, native Windows and
long-session stability remain unverified. Full shared-world co-op requires
further game-specific replication and authority work beyond this visual
prototype.

Do not commit credentials, the `sshpw` file, game executables, assets, saves or
runtime logs.
