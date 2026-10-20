# How it works

This document explains how the mod loads into Days Gone, how it finds and
drives the game engine, and how each synchronized feature works. To install
and play, see the [README](../README.md). To build and test, see
[DEVELOPMENT.md](DEVELOPMENT.md).

> The analysis, code and this document were produced largely with AI coding
> assistants, under human direction, and checked against the running game.
> Addresses and offsets apply **only** to the supported game build.

## Overview

Days Gone runs on Bend Studio's fork of **Unreal Engine 4.10.1**. The mod is a
Windows DLL (`DaysGoneTogether.asi`) that runs inside the game process.

```text
DaysGone.exe
  └─ version.dll (Ultimate ASI Loader proxy)
       └─ DaysGoneTogether.asi → DllMain / InitializeASI
            ├─ load DaysGoneTogether.ini next to the .asi
            ├─ check the executable (PE timestamp + image size)
            ├─ check the ProcessEvent prologue bytes
            ├─ install an inline hook on UObject::ProcessEvent
            └─ start the Direct3D 11 overlay setup thread

every ProcessEvent call on the game thread
  └─ HookedProcessEvent
       ├─ Tick() (on the local HUD frame, at most 60 Hz, re-entrancy guarded)
       │    ├─ resolve engine globals and classes (once)
       │    ├─ find the local player pawn
       │    ├─ send state + poses, receive and validate packets
       │    ├─ update avatar, held item, bike, passenger, enemies
       │    └─ world time/weather, menu actions, teleport
       └─ original ProcessEvent
```

No game files are changed on disk. All engine work happens on the game thread
through the engine's own reflection system.

## 1. Target

| Item | Value |
|---|---|
| Game | Days Gone, Steam app 1259420 |
| Build | 19221447; PE timestamp `0x68754d46`, SizeOfImage `0x093f0000` |
| Executable SHA-256 | `17c2fca01d566914147a6d4924d038cb1f5a822c2829e4c45862e2bf837730b3` |
| Engine | Unreal Engine 4.10.1, Bend fork |

The plugin refuses to install if the timestamp, image size or hook-site bytes
differ. After a game update the mod disables itself rather than patching the
wrong code.

## 2. Reverse engineering

### 2.1 Static analysis

The executable was analysed offline. No debugger was attached to the game.

- **PE parsing:** [`tools/pe_image.py`](../tools/pe_image.py) validates the PE
  structure and checks the executable's SHA-256 and Steam build ID against
  [`config/steam-19221447.json`](../config/steam-19221447.json). The `.text`
  section is not encrypted, so it can be disassembled directly.
- **Disassembly:** [Capstone](https://www.capstone-engine.org/) decodes
  functions using the ranges in the PE exception directory (`.pdata`).
- **String anchors:** Unreal keeps reflection type names such as
  `ByteProperty` and `ObjectProperty` as literal strings.
  [`tools/engine_discovery.py`](../tools/engine_discovery.py) finds them and
  lists the code that references them through RIP-relative operands. This
  narrows the search to the core UObject and actor-spawning code. These are
  *candidates* only; the tool doesn't prove an address is safe to call.

### 2.2 Public Unreal Engine knowledge

UE4's source is available to licensees, and its core layouts (`UObject`,
`UStruct`, `UFunction`, `UProperty`, `FName`, `FUObjectArray`) and the logic of
`ProcessEvent` are widely documented. An SDK dump of an **older** build,
published by the community project
[DaysGoneUnrealModLoader](https://github.com/Mythical-Github/DaysGoneUnrealModLoader),
served as a reference for class names, UFunction names and parameter names.
Its addresses don't apply to this build, and no code or assets from it are
used.

### 2.3 Located in the binary

| Item | Location | Identification |
|---|---|---|
| `UObject::ProcessEvent` | RVA `0x1AC2A20` | Matches UE 4.10 `ProcessEvent`: tests `FunctionFlags` (+0x88) for `FUNC_Native` (0x400), reads the script size (+0x50), virtual calls at vtable +0x1E8/+0x1F0. Its 19-byte prologue is position-independent |
| `GUObjectArray` | RVA `0x4C12290` | `Objects*` +0x10, `Max` +0x18, `Num` +0x1C; 16-byte items with flags at +8 and serial at +0xC |
| `GNames` | found at runtime (§3.2) | |
| `FNameEntry` string | +0x12 | Stock UE 4.10 uses +0x10; Bend added a 16-bit field |
| `UProperty::Offset_Internal` | detected at runtime, +0x50 (§3.3) | |

Structure offsets used by the reflection layer ([`ue.hpp`](../src/coop/ue.hpp)):

| Structure | Field | Offset |
|---|---|---|
| `UObject` | vtable / ObjectFlags / InternalIndex / Class / Name / Outer | 0x00 / 0x08 / 0x0C / 0x10 / 0x18 / 0x20 |
| `UField` | Next | 0x28 |
| `UStruct` | SuperStruct / Children | 0x30 / 0x38 |
| `UFunction` | FunctionFlags / ParmsSize (u16) | 0x88 / 0x90 |
| `UProperty` | ArrayDim / ElementSize / Offset_Internal | 0x30 / 0x34 / 0x50 |
| `FNameEntry` | Index (bit 0 = wide) / HashNext / string | 0x00 / 0x08 / 0x12 |
| `TNameEntryArray` | 128 chunk pointers × 16384 entries; NumElements | 0x000; 0x400 |

### 2.4 Runtime discovery

Some facts only exist at runtime. With `Discovery=1` the plugin logs the local
pawn's class chain and components, writes loaded `AnimSequence`s to
`DaysGoneTogether-anims.txt` and lists classes related to world state, AI and
spawning. F6 dumps the current pawn again. The separate `MissionDiscovery=1`
option logs unique mission/save-related `ProcessEvent` signatures and reflected
parameter layouts without recording parameter values or changing behavior.
It showed that the main menu runs inside an already-loaded world and that
Continue loads a save in place; see the save lifecycle in
[DEVELOPMENT.md](DEVELOPMENT.md).
This revealed, for example:

- the player is `BendDefaultPlayerPawn_C` with `BendDefaultPlayerController_C`;
- Deacon is **modular**: a head plus several skinned parts (jacket, jeans,
  hat, forearms, bag, …) that follow the main mesh's pose;
- the open world is managed by `BP_BendWorldManagerV3_C`, based on the native
  `BendWorldManager`;
- enemies derive from `BendAIPawn` (for example `SwarmerPawn_C`), and many of
  those objects are pooled, inactive actors.

### 2.5 Engine quirks

- Bend's `SetSkeletalMesh` has no `bReinitPose` parameter, unlike stock UE4.
- Copying a mesh without its materials gives a grey avatar. Materials have to
  be copied slot by slot.
- Loading a save swaps Deacon's meshes and plays a fade-in, and an avatar
  copied during that phase stays invisible. The plugin detects a mesh change
  or a jump of more than 50 m, waits 5 s, then rebuilds the avatar.
- `GetWorldWeather()` reports 0 even after a weather change. The
  `WorldWeather` byte property holds the real value.
- `ApplyDamage` can return 0 although health was reduced, so tests check
  health rather than the return value.
- `ProcessEvent` is also called from non-game threads, so the hook only does
  gameplay work on the thread that called it first.
- Under Proton, the ASI loader only loads with a `version=native,builtin` DLL
  override.

## 3. Engine access

### 3.1 The ProcessEvent hook

`ProcessEvent` runs every Blueprint event and reflected function call, many
times per frame on the game thread. `InstallHook` in
[`coop.cpp`](../src/coop/coop.cpp):

1. compares the 19 bytes at `base + 0x1AC2A20` with
   `40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00`
   (`push rbp, rsi, rdi, r12–r15; sub rsp, 0xF0`) and stops if they differ;
2. builds an executable trampoline from those bytes followed by an absolute
   `jmp` back to `ProcessEvent + 19`;
3. overwrites the prologue with a `jmp` to `HookedProcessEvent`, padded with
   NOPs.

The hook remembers the first calling thread as the game thread. On that thread
it runs `Tick()` with a re-entrancy guard, because `Tick()` calls
`ProcessEvent` itself, and then always forwards to the original. Visual updates
follow the HUD's `ReceiveDrawHUD`. A timed fallback keeps networking alive
during loading screens.

### 3.2 Finding GNames

`InitNames` scans the executable's `.data` section and treats each 8-byte slot,
or the pointer stored in it, as a candidate name table. A candidate is accepted
when entry 0 is `None`, entry 1 is `ByteProperty` and the element count is
plausible. Each read is checked with `VirtualQuery` first.

### 3.3 Finding the property offset field

`GameplayStatics.GetPlayerPawn(WorldContextObject, PlayerIndex) → ReturnValue`
has parameters at offsets 0, 8 and 0x10. The plugin tries each candidate field
offset between 0x40 and 0x74 and keeps the one for which all three parameters
report those offsets. That is +0x50 on this build.

### 3.4 Reflection layer

[`ue.hpp`](../src/coop/ue.hpp) provides:

- `FindClass`, `FindObject` and `FindDefaultObject`, walking `GUObjectArray`,
  with validated weak caches;
- `FindChild`/`FindFunction`, walking `Children` and `Next` up the
  `SuperStruct` chain;
- `IsA`, and `IsLive`: an object must be readable, its array slot must point
  back to it, and it must not be PendingKill or Unreachable;
- `Call`, which sizes a parameter buffer from `ParmsSize`, writes each named
  parameter at its reflected offset with a size check, and invokes the
  original `ProcessEvent`. A missing function or parameter rejects the call
  and logs it; nothing is written.

```cpp
Call move(avatar, "K2_SetActorLocationAndRotation");
move.set("NewLocation", pos).set("NewRotation", FRotator{0, yaw, 0})
    .setBool("bSweep", false).setBool("bTeleport", true).invoke();
```

Engine functions are called **by name**. Only `ProcessEvent` and
`GUObjectArray` have hard-coded addresses.

## 4. Networking

### 4.1 Peering

One non-blocking UDP socket ([`net.cpp`](../src/coop/net.cpp)). In host mode
(`PeerHost` empty) the first valid sender becomes the partner. In join mode
the plugin sends to the configured host. Either way, the peer's address is
then pinned until the game restarts. A peer that has been silent for 5 s
counts as disconnected. A new session ID (from `BCryptGenRandom`) counts as a
reconnect.

Both transports implement the same small `Transport` interface: send a
datagram, receive the partner's datagrams, and poll. The rest of the plugin
does not know which one is active.

### 4.1.1 Steam transport (experimental, untested in the game)

With `Transport=steam`, [`steam.cpp`](../src/coop/steam.cpp) replaces the UDP
socket. No Steamworks SDK code is bundled or linked:

- The game already loads `steam_api64.dll` and initializes Steam. The plugin
  takes the loaded module with `GetModuleHandle` and resolves four exports:
  `SteamAPI_GetHSteamUser`, `SteamInternal_FindOrCreateUserInterface`,
  `SteamAPI_RegisterCallback` and `SteamAPI_RunCallbacks`. `DaysGone.exe`
  contains the same export names as strings, which shows the game's DLL
  provides them.
- Interfaces are requested by version string. The running Steam client serves
  them, so versions newer than the game's SDK are available, for example
  `SteamNetworkingMessages002`. If that is missing, it falls back to the
  legacy `SteamNetworking006` P2P API.
- Methods are called through vtable slots taken from the public SDK headers
  ([`steam_abi.hpp`](../src/coop/steam_abi.hpp)). MinGW and MSVC share the x64
  calling convention, with one exception: MSVC returns a `CSteamID` from a
  member function through a hidden pointer after `this`. Those calls pass that
  pointer explicitly. Struct layouts are checked with `static_assert`.
- **Discovery.** The host creates a friends-only lobby limited to two members
  and sets lobby data `dgt` to the protocol version. A joiner leaves any lobby
  whose `dgt` doesn't match. A friend joins in one of three ways:
  - a Steam overlay invite, through the `GameLobbyJoinRequested_t` callback;
  - the `+connect_lobby <id>` launch argument Steam adds when the game wasn't
    running;
  - the menu, which lists friends playing the same app ID whose
    `FriendGameInfo_t` reports a lobby.
- **Results without threads.** `CreateLobby` and `JoinLobby` results are
  polled from the game thread with `ISteamUtils::GetAPICallResult`. Callbacks
  use hand-built `CCallbackBase` objects. They may fire on whichever thread
  the game runs callbacks on, so they only queue events under a mutex. Lobby
  membership is polled once a second.
- **Traffic.** Datagrams go to the other lobby member as unreliable,
  no-Nagle messages on channel 0. Received messages from anyone except the
  current partner are dropped and released. Session requests are accepted
  only from lobby members. Steam authenticates the sender and encrypts the
  traffic, and its relays avoid exposing IP addresses or forwarding ports.
- **Roles.** The lobby owner is the host for world time, weather and enemies.
  In LAN mode, the player without `PeerHost` is the host.

### 4.2 State packet (protocol v6)

[`protocol.hpp`](../src/coop/protocol.hpp) defines a fixed 144-byte
little-endian packet, sent at 30 Hz:

| Field | Type | Notes |
|---|---|---|
| magic, version | u32, u16 | `"DGCO"`, 6 |
| flags | u16 | HasPawn, InVehicle, HasBike, Passenger, FriendlyFire, AcceptPassengers, Alive, SyncBikes, WorldState |
| seq, timeMs | u32 each | wrapping sequence; sender's frame clock |
| pos[3], yaw, pitch, vel[3] | f32 | finite, within ±1e8 |
| name | char[16] | |
| session, targetSession, consent, targetConsent | u32 each | connection and permission scope |
| attackId, attackAck, damage, attackKind | u32, u32, f32, u32 | friendly-fire delivery |
| bikePos[3], bikeRotation[3] | f32 | |
| jacket, trousers, capColor, cap | u8 each | |
| health | f32 | |
| actions, weaponType, shotSequence | u8, u8, u32 | aiming/crouch/reload, weapon category, shots |
| worldTimeOfDay, worldRelativeTime, worldWeather, worldTransitionRate | f32, f32, u8, u8 | host world state |

Decoding rejects a packet with the wrong size, magic, version or flags, or
with non-finite or out-of-range values. Each frame processes at most 64
packets. A state packet with the right magic but another version is logged
once, so a version mismatch is visible in the log.

### 4.3 Additional datagrams

The same socket carries four more types, each identified by its own magic:

| Magic | Content | Rate |
|---|---|---|
| `DPOS` | Compressed skeleton pose: body, bike or enemy | body and bike 20 Hz (`PoseRate`); enemies 15 Hz |
| `DMGR` | Held item: mesh asset path, socket and offset | on change and every 0.5 s |
| `DMEN` | Enemy appearance: up to four mesh asset paths | every 1 s per enemy |
| `DGSV` | Experimental save transfer: request, offer, chunk pull, 1 KB chunk | only while a guest downloads the host's save |

A pose datagram (version 2) carries up to 64 bones, so it never exceeds
1,132 bytes. Each bone has a 32-bit hash of its name (bones are matched by
name, not by index), a "smallest-three" compressed quaternion and, only when
needed, a 16-bit translation in 0.02 cm steps. A bone is 11 bytes without a
translation and 17 bytes with one.

Most animated bones only rotate, so the sender includes a translation only:

- in a keyframe, once per second per stream;
- for 250 ms after that bone's translation changed, so a single lost datagram
  heals.

The receiver remembers the last translation per bone. A bone whose translation
it has never received is skipped until the next keyframe, and the receiving mesh
keeps its current value for it.

The player body skips its face rig: every bone below `head` and any bone with a
face-related name (eye, jaw, lip, brow and similar). Hands and fingers are
still streamed. The partner therefore shows a neutral face. `PoseFace=1`
streams the full skeleton again.

These measures replace the earlier format, which sent all 529 body bones with
translations at 30 Hz (about 300 kB/s each way, measured). The new expected
cost is well under 100 kB/s per direction for body and bike. That estimate
still has to be confirmed in a live session with the `perf:` log lines.

### 4.4 Motion smoothing

[`motion.hpp`](../src/coop/motion.hpp) keeps received samples on the
**sender's** timeline and plays them back with an adaptive delay of 80-250 ms.
It uses monotone cubic Hermite interpolation and shortest-path angles, and
extrapolates for at most 150 ms with velocity capped at 70 m/s. Teleports over
30 m and gaps over 2 s reset the timeline. Timestamps come from
[`frameclock.hpp`](../src/coop/frameclock.hpp), which advances with the engine's
per-frame real time instead of the time a draw call happens to run. That
removes several milliseconds of jitter per sample.

## 5. Features

### 5.1 Partner avatar and animation mirroring

The partner is a visual stand-in, not a second player pawn:

1. A `SkeletalMeshActor` is spawned with collision off. It gets the local
   player's body mesh and materials, plus one attached actor per modular part,
   slaved with `SetMasterPoseComponent`.
2. The body is a `PoseableMeshComponent`. The sender reads the
   **evaluated** bone transforms that the engine already computed for
   rendering. The receiver interpolates two poses on the motion timeline and
   writes them into the copy. Every state-machine clip, additive layer and IK
   result therefore appears as the partner sees it.
3. If no pose arrives for 0.5 s, the avatar falls back to speed-picked clips
   (idle, walk, run, sprint) from the INI.
4. The avatar is hidden when the partner times out.

Because it reuses your own meshes, the partner looks like your Deacon, tinted
with the partner's chosen colors.

### 5.2 Held items

The sender reads `GetEquippedItemCurrentlyInHand`, the item's mesh asset, the
socket it is attached to and its offset from that socket. The receiver finds
the same asset with `FindObject`, which never loads from disk, and attaches it
to the avatar's socket. Fresh meshes in Days Gone fade in through per-instance
materials, so the receiver borrows the materials of a visible component that
shows the same asset. That lookup is cached per asset. If the asset isn't
loaded on the receiver, nothing is shown.

Writing bones directly skips Unreal's child-attachment update, so anything
attached to a socket on a posed mesh would freeze. `FollowSocket` places such
children in world space every frame.

### 5.3 Motorcycles and passengers

The player's bike is found through `Pawn.GetVehicle` while mounted, or through
`GameMode.GetVehicle(CurrentPlayerVehicle)`, because `CurrentPlayerVehicle`
is a class rather than an instance. The proxy clones the chassis and its
modular parts. Parts that share the chassis skeleton are slaved to it; the
rest follow their sockets (§5.2). The chassis skeleton is streamed as a pose,
so steering, suspension and wheels match the partner's bike. Bike physics are
not shared.

Passenger mode reads the socket of the first non-driver seat from the bike's
`SeatsComponent.SeatSetups` (`BendSeatSetup`). It aligns an independent posed
body's hips/pelvis bone with that socket and moves the hidden local character
with it, for the camera and health systems. On the driver's machine the
passenger body is anchored to the driver's own bike socket, to avoid a second
network delay. Boarding
captures movement mode, visibility, collision and the save-block flag, and
restores each of them on exit, dismount, permission removal or disconnect. A
manual exit is checked with ground and capsule traces.

### 5.4 Appearance

Garment material slots are found by following material `Parent` chains.
Recoloring the shaders' tint parameter had no visible effect. Instead, a
private `CanvasRenderTarget2D` redraws the base color texture multiplied by the
palette color, keeping normal maps intact. Each tinted material is a fresh
material instance, so colors never leak to the other player. Original
materials are kept alive through the object array's RootSet flag while the
plugin needs them, because a raw C++ pointer doesn't stop Unreal's garbage
collector.

### 5.5 Settings overlay

The menu uses [Dear ImGui](https://github.com/ocornut/imgui) with its DX11
backend. A setup thread creates a dummy D3D11 swap chain to find the shared
`Present` and `ResizeBuffers` vtable slots and replaces them. The game thread
publishes an immutable frame under a mutex; `Present` draws it and restores
D3D state. Input comes from the keyboard, the mouse and XInput (`xinput1_3`,
`xinput1_4`, `xinput9_1_0`). Gameplay input is blocked while the menu is open.

### 5.6 Friendly fire

Firearm attacks are detected with `OnWeaponFired_Event`, melee attacks by
sampling the pawn's `m_bIsInMeleeAttack` state and the weapon's collision
window. The range limit is 3.5 m for melee and 500 m for gunshots. Aim and a
world trace are checked against the partner as rendered on the shooter's
screen. Damage is the midpoint of the weapon's configured range, so skills,
criticals and falloff are not reproduced. The receiver applies it with the
game's own `ApplyDamage`.

Attacks are queued (at most 64), retransmitted until acknowledged,
deduplicated and expired after 3 s. Consent epochs invalidate pending attacks
when either player turns friendly fire off, dies, times out or reconnects.
This is consent and reliability handling, not authentication.

### 5.7 Time and weather

The host reads its time with `GetWorldTimeOfDay`/`GetWorldTimeFractional`
and its weather from the `WorldWeather` property, every 0.5 s, outside missions
and cinematics, and sends them. The joining player:

- captures its own time and weather once;
- applies the host's time when the difference is more than 0.02 h, using
  `SetWorldTimeOfDay`;
- applies the host's weather with `SelectWorldWeather` when no weather
  transition is in progress;
- pauses while either game is in a mission or cinematic;
- restores its captured values on disconnect or a world change.

None of this writes save data.

### 5.8 Host enemies

The host scans the object array in time-sliced batches for live `BendAIPawn`s
within 80 m of either player, at most 16. It streams their skeletons and
appearance. The joining player builds a posed, non-colliding replica for each
one and removes it after 1.5 s of silence or when its health reaches 0.
Replicas have no AI and can't be damaged. Real shared combat would need an
authoritative enemy model, which doesn't exist yet.

## 6. Safety measures

- Exact build and prologue checks before any patching.
- Engine work only on the game thread, inside the engine's own call path.
- `VirtualQuery` before reading unknown pointers; `IsLive` before using objects.
- Reflection calls verify function and parameter names, sizes and offsets.
- Strict packet validation; bounded per-frame work; rate-limited spawn retries.
- A `FATAL` log entry disables the mod's gameplay work and the game keeps
  running.
- Debug tools are off by default, local only, and limited to an allowlist of
  commands (see [DEVELOPMENT.md](DEVELOPMENT.md#debug-tools)).
- The experimental guest save sandbox (`GuestSaveGuard=1`) hooks the game's
  file imports so no write can reach the real `SaveGames` folder. Saves received
  from the host are checked with SHA-256 and strict file-name rules before the
  game can see them (see
  [DEVELOPMENT.md](DEVELOPMENT.md#host-to-guest-save-transfer-sharesave--guestsaveguard-experimental)).

## 7. Limitations

- Each player runs their own single-player world. Missions, saves, inventory
  and loot are not shared. The experimental save transfer only lets the guest
  start from a copy of the host's last save.
- Enemy replicas are visual. The joining player can't fight the host's enemies
  and still has their own.
- No projectile or muzzle-flash replication. Friendly-fire damage is an
  approximation.
- Motorcycle physics, fuel, damage and upgrades are not shared.
- Two players only. LAN mode is for trusted networks only: its protocol is
  neither encrypted nor authenticated. The Steam transport is encrypted and
  authenticated by Steam, but it has not been tested in the game yet.
- Only one game build is supported. Native Windows is untested.
