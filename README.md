![Theys Gone](./docs/theys_gone.avif)

[![CI](https://github.com/DrSkunk/theys-gone/actions/workflows/ci.yml/badge.svg)](https://github.com/DrSkunk/theys-gone/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/DrSkunk/theys-gone?include_prereleases&sort=semver)](https://github.com/DrSkunk/theys-gone/releases)
[![Nexus Mods](https://img.shields.io/badge/Nexus_Mods-Theys_Gone-orange)](https://www.nexusmods.com/daysgone/mods/1019)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Theys Gone** is an experimental two-player co-op mod for **Days Gone
(PC)**. It runs as an ASI plugin inside the game. The host's save is the one
that counts: the joining player loads it, fights the host's enemies and helps
the host progress. Host and join sessions through Steam or your local network.

> [!IMPORTANT]
> This is **not** full campaign co-op. Each player still runs their own copy of
> the game. The joining player starts from the host's save and is resynced when
> the host's story moves on, but missions, inventory and loot are not mirrored
> live. The UDP/LAN transport is unauthenticated and should only be used on a
> trusted local network. Back up your saves before you try it.

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
| Passenger riding | Walk up to your partner's bike and hold the native **Ride with** prompt (experimental) |
| Host-first progress | The joining player plays on a copy of the host's save; their own saves are never touched |
| Story sync | When the host saves with new story progress, the joining player is reloaded onto it |
| Shared enemies | The host's hostile AI, up to 16 within 80 m, appears for the joining player instead of their own |
| Shared combat | The joining player's shots and melee damage the host's enemies; those enemies attack and damage the joining player |
| Time and weather | The joining player's time of day and weather follow the host |
| Appearance | Seven color presets for jacket, trousers and cap, and an option to hide the cap |
| Friendly fire | Optional. Both players must opt in each session |
| Teleport | Jump next to your partner |
| In-game menu | Settings overlay usable with keyboard, mouse or gamepad |

### What is not synchronized

- Live mission state, inventory, loot and doors. The joining player's story
  is only resynced when the host saves.
- Camp NPCs and companions: each player keeps their own.
- Ranged enemy attacks on the joining player. Only melee swings at the
  joining player are forwarded, and their damage per enemy type is an estimate.
- Projectiles, muzzle flashes and hit effects.
- Motorcycle physics, fuel, damage and upgrades.
- Outfit models. Your partner is drawn with *your* Deacon outfit, tinted with
  *their* chosen colors.

## Requirements

- Days Gone on **Steam, build 19221447**. The mod checks the executable and
  disables itself on any other build, for example after a game update.
- Windows, or Linux / Steam Deck with Proton. It has been tested under Proton
  on a desktop Linux PC and a Steam Deck. Native Windows has not been tested.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (x64) to load the plugin.
- Two PCs with the same mod version. Connect over a trusted LAN or through
  Steam. Two-account Steam sessions have been tested between separate
  machines; Internet/NAT traversal has not been independently verified.

## Install

1. **Back up your saves.** They are in `%LOCALAPPDATA%\BendGame\Saved\` on
   Windows. Under Proton they are in
   `steamapps/compatdata/1259420/pfx/drive_c/users/steamuser/AppData/Local/BendGame/Saved/`.
2. Download `TheysGone-<version>.zip` from
   [Nexus Mods](https://www.nexusmods.com/daysgone/mods/1019) or
   [GitHub Releases](https://github.com/DrSkunk/theys-gone/releases).
   Use **Manual Download** on Nexus; Vortex installation is not supported.
3. Install Ultimate ASI Loader x64 as `version.dll` in the game's
   `BendGame/Binaries/Win64/` folder, next to `DaysGone.exe`. Skip this step if
   you already have an ASI loader there, and don't overwrite other mods.
4. Copy `TheysGone.asi` and `TheysGone.ini` from the zip into that same
   folder. Both files must stay side by side.
5. **Linux / Steam Deck only:** Wine must load the proxy `version.dll`. In
   Steam, open the game's *Properties → Launch Options* and set:

   ```sh
   WINEDLLOVERRIDES="version=n,b" %command%
   ```

   If you already have launch options, add to them instead of replacing them.

To verify a download:

```sh
gh attestation verify TheysGone-<version>.zip --repo DrSkunk/theys-gone
```

The release also includes a `.sha256` checksum file.

## Set up a session

Open **CO-OP** in the game's title menu or pause menu. These are native game
widgets, not the F8 settings overlay. You can host before loading a save or
open a session while already playing. Sessions currently hold **two players**.

### LAN

Choose **HOST LAN GAME**. On the other PC choose **JOIN LAN GAME**, then select
the host from the local-network list. Use **CLOSE SESSION** / **LEAVE SESSION**
to disconnect without restarting the game.

For automatic LAN hosting/joining instead of choosing in the menu, set
`Transport=udp` and configure:

**Host:** leave `PeerHost` empty.

```ini
[Coop]
Port=27777
Transport=udp
PeerHost=
PlayerName=Host
```

**Joining player:** set `PeerHost` to the host's LAN IP address or hostname.

```ini
[Coop]
Port=27777
Transport=udp
PeerHost=192.168.1.50
PeerPort=27777
PlayerName=Partner
```

- Allow inbound **UDP 27777** (or your `Port`) through the host's firewall,
  for example with `sudo ufw allow from 192.168.1.0/24 to any port 27777 proto udp`.
  **Never** forward this port to the Internet: the protocol has no encryption
  or authentication.
- The host loads their save. The joining player is given the host's save
  automatically; see below.
- Settings are read when the game starts. Restart after editing the INI;
  switching sessions from CO-OP does not require a restart.

#### Playing on the host's save

With `HostSave=1` (the default), the host's save is the one that counts:

1. The host loads their game. Their newest save is sent to whoever joins.
2. The joining player connects from the title or pause menu. From that moment
   every save write on their PC goes to a throw-away sandbox
   (`TheysGone-guest-saves`, wiped at each join), so their own saves are never
   touched.
3. The host's save is downloaded (a few seconds for about 7 MB on a LAN) and
   loaded. If the joining player is already in the game, it reloads in place.
   They spawn next to the host.
4. Each time the host saves, the host's story state (jobs and missions) is
   compared. If the joining player's story differs, they download the new save
   and are reloaded onto it.

**Nothing the joining player does in that session is kept.** They are there to
help the host progress. Set `HostSave=0` on both PCs to have everybody play
their own save instead.

### Steam sessions (experimental)

> [!WARNING]
> Two-account Steam sessions have been tested between separate machines.
> Internet/NAT traversal has not been independently verified. Send your
> `TheysGone.log` if connection or gameplay fails.

Steam mode uses Steam Networking, so it does not require manual port
forwarding. Steam provides authenticated, encrypted transport and can use
NAT traversal or relaying. Actual Internet-path behavior has not been
independently verified. Leave `Transport=menu` (the default):

```ini
[Coop]
Transport=menu
PlayerName=
Visibility=friends
```

1. **Host:** choose **CO-OP → HOST ONLINE GAME**, then **INVITE FRIENDS** to
   open Steam's invite dialog. **VISIBILITY** cycles between invite-only,
   friends-only, and public; it can also be changed from the pause menu.
2. **Guest:** accept the Steam invite, choose **JOIN A FRIEND**, or use
   **FIND PUBLIC GAMES** for public mod lobbies.
3. At the title menu, **START GAME** invokes the game's normal Continue action
   on this PC. It does not remotely start the other player's game.
4. Steam persona names are used by default. Connection events appear in the
   lobby log and as native HUD notifications during gameplay. This is an
   event log, not typed text chat.

Only traffic from the other lobby member is accepted. `Port` and `PeerHost`
do not control Steam connections. Epic Online Services is not implemented.
The lobby owner is the host: their save, story and enemies are authoritative.

## Controls

| Action | Keyboard / mouse | Gamepad (XInput) |
|---|---|---|
| Open or close gameplay settings | F8 | View/Back + Y |
| Select a row | Up/Down or click | D-pad Up/Down |
| Change or activate | Left/Right, Enter or click | D-pad Left/Right or A |
| Close | Esc | B |
| Join or leave the partner's bike | F9 | View/Back + X |
| Teleport next to the partner | F7 (`TeleportKey`) | Menu |

The game window must be focused. The F8 settings overlay does not pause the
game. Session hosting/joining is in the native title/pause **CO-OP** menu.

On a **Steam Deck**, the mod reads XInput, not Steam Input actions. If the
gamepad shortcuts do nothing, pick the *Gamepad With Joystick Trackpad*
template under *Controller Settings → Current Layout → Templates*.

### Gameplay settings (F8)

| Row | Effect |
|---|---|
| Jacket / Trousers / Cap color | Original, Crimson, Ocean, Forest, Sand, Charcoal, Violet |
| Cap | Show or hide your cap |
| Sync motorcycles | Show your partner's bike and send yours |
| Allow a passenger | Let your partner ride on your bike |
| Friendly fire | Real, lethal damage. Both players must turn it on, and it needs a second confirmation |
| Sync host's enemies | Host: send nearby enemies and take the joining player's hits. Joining player: replace your own enemies with the host's |
| Join / leave partner's bike | Same as F9 |
| Teleport to partner | Same as F7 |
| Steam lobby | Steam mode only: host a lobby, or leave the current one |
| Invite a Steam friend | Steam mode only: opens the Steam overlay invite dialog |
| Join a Steam friend | Steam mode only: Left/Right picks a friend who is hosting, A/Enter joins |

Menu changes are saved to the INI right away, except friendly fire. Friendly
fire starts off in every session and is never saved.

### Passenger riding

Both players need *Sync motorcycles* on, and the driver needs *Allow a
passenger*. The driver gets on their bike and stops. The passenger walks up
to it and holds the game's own interact button on the **Ride with
&lt;partner&gt;** prompt (F9 also works). The passenger is attached to the bike
as a visual rider, and it isn't the game's own two-seat vehicle code. The
passenger can't attack, and saving is blocked while riding. Hold **Get off**
(or press F9) to dismount. An exit is refused when there is no safe ground to
stand on.

## Configuration reference

All keys are in the `[Coop]` section of `TheysGone.ini`.

| Key | Default | Meaning |
|---|---|---|
| `Port` | `27777` | Local UDP port |
| `PeerHost` | *(empty)* | With `Transport=udp`: empty = host; otherwise host IP or hostname |
| `PeerPort` | same as `Port` | Host's UDP port (joining player only) |
| `PlayerName` | *(empty)* | Uses the Steam persona name; optional LAN override (state packets hold 15 UTF-8 bytes) |
| `TeleportKey` | `118` (F7) | Windows virtual-key code for teleport |
| `JacketColor`, `TrousersColor`, `CapColor` | `0` | 0 Original … 6 Violet |
| `ShowCap` | `1` | Show your cap |
| `SyncMotorcycles` | `1` | Bike sync |
| `SyncEnemies` | `1` | Host enemy sync |
| `AllowPassenger` | `1` | Allow a passenger on your bike |
| `Discovery` | `0` | Developers: log engine details; F6 dumps the local pawn |
| `MissionDiscovery` | `0` | Developers: trace mission/save function names and parameter layouts only; see [DEVELOPMENT.md](docs/DEVELOPMENT.md) |
| `MenuDiscovery` | `0` | Developers: F6 dumps native menu classes/widget references and enables UI event tracing |
| `HostSave` | `1` | Host-first progress: the host shares their save; the joining player's save writes go to a throw-away sandbox and the host's save is loaded (see [Playing on the host's save](#playing-on-the-hosts-save)). `0`: everybody plays their own save |
| `DebugTools` | `0` | Developers: enable test tools (see [DEVELOPMENT.md](docs/DEVELOPMENT.md)) |
| `AnimIdle`, `AnimWalk`, `AnimRun`, `AnimSprint` | Deacon clips | Fallback animations used when no pose stream arrives |
| `PoseRate` | `20` | Player and bike pose updates per second (10-30). Lower saves bandwidth |
| `PoseFace` | `0` | `1` also streams the face rig (about 280 more bones, much more bandwidth) |
| `Transport` | `menu` | Choose Steam/LAN in CO-OP; `udp` automatically hosts/joins LAN using the INI |
| `Visibility` | `friends` | Steam: `private` (invite only), `friends`, or `public`; adjustable while hosting |
| `SteamPumpCallbacks` | `0` | `1` also pumps Steam callbacks from the mod. Try it if invites never arrive |

## Troubleshooting

The plugin writes `TheysGone.log` next to itself. A healthy start looks like this:

```text
Theys Gone … starting (protocol v6)
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

Close the game and delete `TheysGone.asi`, `TheysGone.ini` and the
`TheysGone*.log`/`.txt` files. Remove `version.dll`, and the launch option,
only if no other mod uses the ASI loader.

Upgrading from the older *Days Gone Together* builds: delete the old
`DaysGoneTogether.*` files first. Settings are not migrated, and old builds
cannot connect to Theys Gone.

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
