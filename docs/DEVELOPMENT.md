# Development

## Prerequisites

| Tool | Used for | Install |
|---|---|---|
| MinGW-w64 (`x86_64-w64-mingw32-g++`, GCC 13 or newer, posix threads) | Plugin build | macOS: `brew install mingw-w64`. Debian/Ubuntu: `apt install g++-mingw-w64-x86-64-posix` and set `CXX=x86_64-w64-mingw32-g++-posix` |
| A native C++20 compiler | Protocol tests | Xcode command-line tools, `g++` or `clang++` |
| Python 3.11+ and [Poetry](https://python-poetry.org/) | Tool tests and analysis | `pipx install poetry` |
| `curl`, `shasum`, `zip` | Fetching Dear ImGui, packaging | usually preinstalled |

## Build and test

```sh
poetry install
make test     # C++ protocol/motion/combat tests + Python unit tests
make build    # package/DaysGoneTogether.asi + DaysGoneTogether.ini
make dist     # dist/DaysGoneTogether-<version>.zip + .sha256
```

The build downloads the pinned Dear ImGui release into `.deps/` and checks its
SHA-256 against [`dependencies/imgui.sha256`](../dependencies/imgui.sha256).
It never touches a game installation.

Local builds report `<version.txt>-dev` in the log. Release builds set
`COOP_VERSION` to the release version. Linking uses `--no-insert-timestamp`,
so the same compiler and sources produce the same binary.

Tests cover the C++ packet decoder and all datagram types, sequence and
timestamp wraparound, friendly-fire consent and retransmission, motion
smoothing under jitter and packet loss, the debug command gate, and the
Python simulator and analysis tools. They don't replace testing in the game.

## Source layout

| Path | Role |
|---|---|
| [`src/coop/dllmain.cpp`](../src/coop/dllmain.cpp) | DLL entry, `InitializeASI` |
| [`src/coop/coop.cpp`](../src/coop/coop.cpp) | Build check, hook, per-frame logic, all features |
| [`src/coop/ue.hpp`](../src/coop/ue.hpp), [`ue.cpp`](../src/coop/ue.cpp) | UE 4.10 reflection layer |
| [`src/coop/protocol.hpp`](../src/coop/protocol.hpp) | State packet |
| [`src/coop/pose.hpp`](../src/coop/pose.hpp), [`skeleton.cpp`](../src/coop/skeleton.cpp) | Pose compression, reading and writing skeletons |
| [`src/coop/gear.hpp`](../src/coop/gear.hpp), [`enemy.hpp`](../src/coop/enemy.hpp) | Held-item and enemy datagrams |
| [`src/coop/motion.hpp`](../src/coop/motion.hpp), [`frameclock.hpp`](../src/coop/frameclock.hpp) | Interpolation and frame clock |
| [`src/coop/combat.hpp`](../src/coop/combat.hpp) | Friendly-fire delivery queue |
| [`src/coop/appearance.cpp`](../src/coop/appearance.cpp) | Garment tinting |
| [`src/coop/menu.cpp`](../src/coop/menu.cpp), [`overlay.cpp`](../src/coop/overlay.cpp) | Menu input and DX11 overlay |
| [`src/coop/net.cpp`](../src/coop/net.cpp), [`log.cpp`](../src/coop/log.cpp) | UDP socket and log |
| [`src/coop/debug_commands.hpp`](../src/coop/debug_commands.hpp) | Debug command allowlist |
| [`tools/`](../tools) | Tests, simulator, debug helper, offline analysis |

[HOW-IT-WORKS.md](HOW-IT-WORKS.md) explains the design.

## Protocol changes

Both players must run the same protocol version. Any change to a packet layout
or its meaning must bump `kProtocolVersion` in `protocol.hpp`, or the
datagram's own version constant, and update `tools/test_protocol.cpp` and
`tools/fake_peer.py`. Mark such commits as breaking (`feat!:`).

## Simulated partner

[`tools/fake_peer.py`](../tools/fake_peer.py) speaks the protocol, so you can
test with a single game.

**On a Linux/Proton machine with systemd:** copy `tools/fake_peer.py` and
`tools/same-pc/*.sh` into one folder on the gaming PC and run:

```sh
./start-test.sh            # follow (default) | circle | still
./stop-test.sh             # stop the simulator and restore the INI
```

`start-test.sh` backs up the INI and points it at `127.0.0.1:27778`. It runs
the simulator as the `dg-together-fakepeer` user service and launches the game.
`DAYS_GONE_DIR` overrides the game folder. Restart the game after stopping.

**By hand,** against a game in host mode:

```sh
poetry run python tools/fake_peer.py --game 192.168.1.50:27777 \
  --port 27778 --offset 300 --radius 150 --speed 150 --duration 60
```

Units are centimeters. `--speed 0` gives a stationary partner and `--follow`
trails the player. Run `--help` for all options.

## Debug tools

The plugin includes local test tools for spawning enemies, invulnerability,
damage probes, and stepping the time and weather. They are meant for checking
features, **not** as a trainer.

Enable them with `DebugTools=1` in the INI and restart. Then send commands with
the standard-library helper, which needs local access to the game folder:

```sh
poetry run python tools/debug_control.py --game-dir "/path/to/BendGame/Binaries/Win64" begin
poetry run python tools/debug_control.py --game-dir "/path/to/BendGame/Binaries/Win64" spawn-single
poetry run python tools/debug_control.py --game-dir "/path/to/BendGame/Binaries/Win64" end
```

| Command | Effect |
|---|---|
| `inspect` | Log a report of native cheat, spawn and horde APIs and AI objects |
| `begin` | Start a test session: capture state, block native saving |
| `god-on` / `god-off` | Make the player invulnerable / vulnerable |
| `spawn-single` | Spawn one swarmer with its native AI, about 6 m ahead |
| `spawn-pack` | Spawn eight independent swarmers (at most 16 owned at a time) |
| `status` | Log player and enemy health, controllers and movement |
| `probe-player` / `probe-enemy` | Apply 5 base damage and check the health change |
| `time-shift` | Host: advance the time of day by 6 hours |
| `weather-next` | Host: switch to the next weather state |
| `cleanup` | Destroy only the enemies this session spawned |
| `end` | Clean up and restore damage and saving state |

Commands go through a file mailbox. The helper atomically writes
`DaysGoneTogether-debug.ini` (`[Debug] Command=…, Request=<new number>`) next to
the ASI. The plugin polls it every 0.5 s on the game thread and writes
`DaysGoneTogether-debug-result.ini`. Requests already present at startup are
ignored. Only the commands above are accepted, and nothing can be sent over
the network.

A session needs a living player on foot, outside missions and cinematics.
It ends automatically on death, a pawn change, mounting a bike, a mission or
cinematic, or after five minutes without a command. Packs are **not** native
hordes. The game's horde functions are tied to saved horde progress, so they
are deliberately not used. Blocking saves reduces risk but is not a sandbox:
use a backed-up or disposable save.

## Offline executable analysis

```sh
make inspect EXE="/path/to/DaysGone.exe" \
  MANIFEST="/path/to/steamapps/appmanifest_1259420.acf" > engine-inspection.json
```

This is read-only. It requires the executable's SHA-256 and the manifest's
build ID to match [`config/steam-19221447.json`](../config/steam-19221447.json),
then validates the PE structure. It reports reflection-string anchors and the
functions that reference them. The results are leads for analysis, not proven
engine addresses. The tool never attaches to a process or writes anything
except its report.

## Releases

Releases are automated with
[release-please](https://github.com/googleapis/release-please-action) and
[Conventional Commits](https://www.conventionalcommits.org/):

| Commit prefix | Effect (before 1.0) |
|---|---|
| `fix:` | patch release |
| `feat:` | minor release |
| `feat!:` / `BREAKING CHANGE:` | minor release (protocol-incompatible) |
| `docs:`, `ci:`, `build:`, `test:`, `chore:` | no release on their own |

1. Merge to `main`. The **Release** workflow opens or updates a release PR
   that bumps `version.txt`, `pyproject.toml` and `CHANGELOG.md`.
2. Merge the release PR. release-please tags `vX.Y.Z` and creates the GitHub
   release.
3. The same workflow builds the ASI with that version, packages
   `DaysGoneTogether-X.Y.Z.zip` with a SHA-256 file, creates a signed build
   provenance attestation and uploads both files to the release.
4. After the GitHub assets are uploaded, the reusable **Nexus Mods** workflow
   downloads that exact archive, verifies its checksum and GitHub attestation,
   and uploads it to [Nexus mod 1019](https://www.nexusmods.com/daysgone/mods/1019).
   It updates the mod version and changelog, keeps older versions available and
   disables mod-manager downloads because Vortex installation is not supported.
   A Nexus failure is reported without removing the GitHub release.

The **CI** workflow runs the tests and uploads a development build as a
workflow artifact on every push and pull request. Dependabot keeps the pinned
GitHub Actions and Python dependencies current.

Release PRs are opened with the default `GITHUB_TOKEN`, so they don't trigger
the CI workflow themselves. Their changes are limited to version and changelog
files.

### Nexus publishing setup and recovery

Configure these in the personal repository's **Settings > Secrets and
variables > Actions**:

- Secret `NEXUSMODS_API_KEY`: a Nexus API key belonging to an account with
  upload permission for the mod. Never put the key in source control or chat.
- Variable `NEXUSMODS_FILE_ID`: `8193009`, the existing file's **API File ID**,
  not its short unique ID.
- Variable `NEXUSMODS_MOD_ID`: `16333760627707`, the **Unique Mod ID**, not the
  page's game-local number `1019`.

The initial mod page and first file must be created on Nexus manually.
Subsequent stable releases publish automatically; missing credentials fail
explicitly. Nexus may still scan or moderate the file before it is downloadable.

For a non-publishing check of an existing release:

```sh
gh workflow run nexus.yml -f tag=v0.2.0 -F publish=false
```

To upload an existing GitHub release after a failed Nexus job, run **Nexus
Mods** from the Actions tab with its release tag and **Upload to Nexus**
checked (or use `-F publish=true`). Check Nexus first: uploading is not
idempotent, so do not rerun an upload that already created the version.
