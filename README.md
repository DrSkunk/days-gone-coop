![Days Gone Together](./docs/days_gone_together.avif)

[![CI](https://github.com/DrSkunk/days-gone-together/actions/workflows/ci.yml/badge.svg)](https://github.com/DrSkunk/days-gone-together/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/DrSkunk/days-gone-together?include_prereleases&sort=semver)](https://github.com/DrSkunk/days-gone-together/releases)
[![Nexus Mods](https://img.shields.io/badge/Nexus_Mods-Days_Gone_Together-orange)](https://www.nexusmods.com/daysgone/mods/1019)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Days Gone Together** is an experimental two-player **LAN co-op mod for Days
Gone (PC)**. It runs as an ASI plugin inside the game and shows your partner,
their motorcycle and the host's nearby enemies in your world.

> [!IMPORTANT]
> This is **not** full campaign co-op. Each player still runs their own
> single-player game, and missions, inventory, loot and saves are not shared.
> Use it on a trusted LAN only, and back up your saves before you try it.

> [!NOTE]
> **AI-assisted project.** Most of this mod was written with AI coding
> assistants: the code, the reverse-engineering analysis and the documentation.
> A human directed the work and tested it in the real game on real hardware.
> See [How it was built](#how-it-was-built).

## Features

| Feature | What you get |
|---|---|
| Partner avatar | Your partner appears as a Deacon in your world, with smoothed movement |
| Animation mirroring | The partner's real skeleton pose is streamed, so walking, strafing, aiming, crouching and riding match their screen |
| Held items | The weapon or tool in your partner's hands is shown in the same hand |
| Motorcycles | The partner's bike is shown with its parts, steering and suspension |
| Passenger riding | Ride on the back of your partner's bike (experimental) |
| Host enemies | The joining player sees the host's nearby enemies, up to 16 within 80 m, as visual copies |
| Time and weather | The joining player's time of day and weather follow the host |
| Appearance | Seven color presets for jacket, trousers and cap, and an option to hide the cap |
| Friendly fire | Optional. Both players must opt in each session |
| Teleport | Jump next to your partner |
| In-game menu | Settings overlay usable with keyboard, mouse or gamepad |

### What is not synchronized

- Missions, story progress, inventory, loot, doors, saves.
- Enemy AI and combat. Host enemies are visual copies for the joining player:
  they can't be fought there, and that player's own enemies still exist.
- Projectiles, muzzle flashes and hit effects.
- Motorcycle physics, fuel, damage and upgrades.
- Outfit models. Your partner is drawn with *your* Deacon outfit, tinted with
  *their* chosen colors.

## Requirements

- Days Gone on **Steam, build 19221447**. The mod checks the executable and
  disables itself on any other build, for example after a game update.
- Windows, or Linux / Steam Deck with Proton. It has been tested under Proton
  on a desktop Linux PC and a Steam Deck. Native Windows has not been tested yet.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (x64) to load the plugin.
- Two PCs on the same LAN, both on the same mod version.

## Install

1. **Back up your saves.** They are in `%LOCALAPPDATA%\BendGame\Saved\` on
   Windows. Under Proton they are in
   `steamapps/compatdata/1259420/pfx/drive_c/users/steamuser/AppData/Local/BendGame/Saved/`.
2. Download `DaysGoneTogether-<version>.zip` from
   [Nexus Mods](https://www.nexusmods.com/daysgone/mods/1019) or
   [GitHub Releases](https://github.com/DrSkunk/days-gone-together/releases).
   Use **Manual Download** on Nexus; Vortex installation is not supported.
3. Install Ultimate ASI Loader x64 as `version.dll` in the game's
   `BendGame/Binaries/Win64/` folder, next to `DaysGone.exe`. Skip this step if
   you already have an ASI loader there, and don't overwrite other mods.
4. Copy `DaysGoneTogether.asi` and `DaysGoneTogether.ini` from the zip into that same
   folder. Both files must stay side by side.
5. **Linux / Steam Deck only:** Wine must load the proxy `version.dll`. In
   Steam, open the game's *Properties → Launch Options* and set:

   ```sh
   WINEDLLOVERRIDES="version=n,b" %command%
   ```

   If you already have launch options, add to them instead of replacing them.

To verify a download:

```sh
gh attestation verify DaysGoneTogether-<version>.zip --repo DrSkunk/days-gone-together
```

The release also includes a `.sha256` checksum file.

## Set up a session

Edit `DaysGoneTogether.ini` on both PCs, then start the game.

**Host:** leave `PeerHost` empty. The first player who connects becomes your
partner for the rest of the session.

```ini
[Coop]
Port=27777
PeerHost=
PlayerName=Host
```

**Joining player:** set `PeerHost` to the host's LAN IP address or hostname.

```ini
[Coop]
Port=27777
PeerHost=192.168.1.50
PeerPort=27777
PlayerName=Partner
```

- Allow inbound **UDP 27777** (or your `Port`) through the host's firewall,
  for example with `sudo ufw allow from 192.168.1.0/24 to any port 27777 proto udp`.
  **Never** forward this port to the Internet: the protocol has no encryption
  or authentication.
- Each player loads their own save. Meet up in the same area, or use teleport.
- Settings are read when the game starts. Restart the game after editing the
  INI or to switch partners.

## Controls

| Action | Keyboard / mouse | Gamepad (XInput) |
|---|---|---|
| Open or close the menu | F8 | View/Back + Y |
| Select a row | Up/Down or click | D-pad Up/Down |
| Change or activate | Left/Right, Enter or click | D-pad Left/Right or A |
| Close | Esc | B |
| Join or leave the partner's bike | F9 | View/Back + X |
| Teleport next to the partner | F7 (`TeleportKey`) | Menu |

The game window must be focused. The menu does not pause the game.

On a **Steam Deck**, the mod reads XInput, not Steam Input actions. If the
gamepad shortcuts do nothing, pick the *Gamepad With Joystick Trackpad*
template under *Controller Settings → Current Layout → Templates*.

### Menu

| Row | Effect |
|---|---|
| Jacket / Trousers / Cap color | Original, Crimson, Ocean, Forest, Sand, Charcoal, Violet |
| Cap | Show or hide your cap |
| Sync motorcycles | Show your partner's bike and send yours |
| Allow a passenger | Let your partner ride on your bike |
| Friendly fire | Real, lethal damage. Both players must turn it on, and it needs a second confirmation |
| Sync host's enemies | Host: send nearby enemies. Joining player: show them |
| Join / leave partner's bike | Same as F9 |
| Teleport to partner | Same as F7 |

Menu changes are saved to the INI right away, except friendly fire. Friendly
fire starts off in every session and is never saved.

### Passenger riding

Both players need *Sync motorcycles* on, and the driver needs *Allow a
passenger*. The driver gets on their bike and stops. The passenger walks up
and presses F9. The passenger is attached to the bike as a visual rider, and
it isn't the game's own two-seat vehicle code. The passenger can't attack, and
saving is blocked while riding. Press F9 again to get off. An exit is refused
when there is no safe ground to stand on.

## Configuration reference

All keys are in the `[Coop]` section of `DaysGoneTogether.ini`.

| Key | Default | Meaning |
|---|---|---|
| `Port` | `27777` | Local UDP port |
| `PeerHost` | *(empty)* | Empty = host. Otherwise the host's IP or hostname |
| `PeerPort` | same as `Port` | Host's UDP port (joining player only) |
| `PlayerName` | `Deacon` | Name shown in the partner's log (max 15 characters) |
| `TeleportKey` | `118` (F7) | Windows virtual-key code for teleport |
| `JacketColor`, `TrousersColor`, `CapColor` | `0` | 0 Original … 6 Violet |
| `ShowCap` | `1` | Show your cap |
| `SyncMotorcycles` | `1` | Bike sync |
| `SyncEnemies` | `1` | Host enemy sync |
| `AllowPassenger` | `1` | Allow a passenger on your bike |
| `Discovery` | `0` | Developers: log engine details; F6 dumps the local pawn |
| `DebugTools` | `0` | Developers: enable test tools (see [DEVELOPMENT.md](docs/DEVELOPMENT.md)) |
| `AnimIdle`, `AnimWalk`, `AnimRun`, `AnimSprint` | Deacon clips | Fallback animations used when no pose stream arrives |

## Troubleshooting

The plugin writes `DaysGoneTogether.log` next to itself. A healthy start looks like this:

```text
Days Gone Together 0.1.0 starting (protocol v5)
ProcessEvent hook installed
GNames via pointer at rva 0x…, … names
network: listening on UDP 27777, …
peer Partner (…) connected
avatar: spawned …
```

| Symptom | Likely cause |
|---|---|
| No log file | The ASI loader isn't loading. Check `version.dll` and, on Proton, the `WINEDLLOVERRIDES` launch option |
| `FATAL: unsupported DaysGone.exe` | Wrong game version. The mod disables itself; wait for an updated release |
| Never connects | Wrong `PeerHost`/port, firewall, a mod version mismatch, or a player hasn't loaded a save |
| Connected but no partner visible | You are far apart (try F7), or the partner just loaded and the avatar is being rebuilt (wait about 5 seconds) |

Any `FATAL` line means the mod switched itself off and the game keeps running
normally.

## Uninstall

Close the game and delete `DaysGoneTogether.asi`, `DaysGoneTogether.ini` and the
`DaysGoneTogether*.log`/`.txt` files. Remove `version.dll`, and the launch option,
only if no other mod uses the ASI loader.

## How it was built

The mod is a C++20 DLL cross-compiled with MinGW-w64. It hooks Unreal Engine
4's `UObject::ProcessEvent` and then drives the game through the engine's own
reflection system, calling functions by name. Game files on disk are never
modified, and no game assets are included. The full write-up covers the
reverse engineering, hooking, networking and synchronization:
[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

**Use of AI:** most of this project was developed with AI coding assistants
(GitHub Copilot and large language models). That covers code, the static and
runtime analysis of the game, tests and documentation. A human chose the
goals, reviewed the results and tested the mod in the real game. AI output
can contain mistakes: check claims against the code and your own tests, and
report anything wrong.

## Development

Building, testing, the simulated partner, debug tools and the release process
are covered in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). Contributions are
welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

Days Gone is © Sony Interactive Entertainment, developed by Bend Studio. This
is an unofficial fan project, not affiliated with or endorsed by Sony or Bend
Studio. It contains no game code or assets. You need your own copy of the game.

Licensed under the [MIT License](LICENSE). Release builds bundle
[Dear ImGui](https://github.com/ocornut/imgui) (MIT); its license is included
in the release archive.
