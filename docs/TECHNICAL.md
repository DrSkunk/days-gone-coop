# How the Days Gone co-op mod works

This document explains how the mod was built: how the game was analysed, what
was reverse engineered, and how the plugin hooks into Unreal Engine at runtime.
For installing and playing, see the [README](../README.md).

## Short answer: yes, it injects into Unreal Engine

Days Gone is built on Bend Studio's fork of **Unreal Engine 4.10.1**. The mod is
a Windows DLL (`DaysGoneCoop.asi`) that runs **inside the game process**:

1. **Loading.** A third-party *ASI loader* is already installed as a proxy
   `version.dll` next to `DaysGone.exe`. Windows (or Proton/Wine with a
   `version=native,builtin` DLL override) loads that proxy instead of the system
   `version.dll`. The proxy then loads every `*.asi` file in the folder and
   calls its exported `InitializeASI` function.
2. **Hooking.** The plugin patches the first 19 bytes of the engine's
   `UObject::ProcessEvent` function with a jump to its own code. This is an
   inline detour, also called a trampoline hook.
3. **Driving the engine.** Inside that hook the plugin is on the game thread,
   so it can safely use the engine's own reflection system. It finds classes,
   reads properties and calls UFunctions such as `SpawnActor`,
   `SetSkeletalMesh` and `PlayAnimation`, the same way Blueprint scripts do.

The original executable and game assets are not patched on disk. Installation
adds the loader, plugin, configuration and license files; the plugin also writes
settings and diagnostic logs. Normal gameplay can update saves. Removing the
co-op `.asi` disables this plugin; remove the loader only if no other mod uses it.

```text
DaysGone.exe
  └─ loads version.dll (ASI loader proxy)
       └─ loads DaysGoneCoop.asi → InitializeASI()
            ├─ checks the exe build (PE timestamp + image size)
            ├─ checks the ProcessEvent prologue bytes
            └─ installs the inline hook on UObject::ProcessEvent
                   │
   every engine ProcessEvent call ───────────────┐
                   ▼                              │
            HookedProcessEvent()                  │
              ├─ first call: remember game thread │
              ├─ on local HUD frame: Tick()       │
              │    ├─ resolve engine (once)       │
              │    ├─ find local player pawn      │
              │    ├─ UDP send/receive (30 Hz)    │
              │    ├─ spawn/move/animate avatar   │
              │    └─ F7 teleport                 │
              └─ call original ProcessEvent ◄─────┘
```

## 1. Target and constraints

| Item | Value |
|---|---|
| Game | Days Gone, Steam app 1259420 |
| Build | Steam build 19221447, PE timestamp `0x68754d46`, SizeOfImage `0x093f0000` |
| Engine | Unreal Engine 4.10.1, Bend fork |
| Test machine | `cachy`: CachyOS, Proton Experimental, RTX 3070 |
| Toolchain | MinGW-w64 cross-compiler on macOS (`x86_64-w64-mingw32-g++`, C++20, static) |

All hard-coded addresses are tied to this one executable. The plugin refuses
to run if the PE timestamp or image size differs, or if the bytes at the hook
site are not what it expects. A game update therefore disables the mod
instead of crashing the game.

## 2. Reverse engineering process

### 2.1 Static analysis of the executable

`DaysGone.exe` was copied off the game PC and analysed offline on the Mac. No
debugger was attached to the running game.

- **PE parsing.** [`tools/pe_image.py`](../tools/pe_image.py) reads the section
  table and verifies the exe's SHA-256 and Steam build ID against
  [`config/steam-19221447.json`](../config/steam-19221447.json). `.text` is
  *not* encrypted, despite an extra `.bind` section, so the code can be
  disassembled directly.
- **Disassembly.** [Capstone](https://www.capstone-engine.org/) disassembled
  functions using the function ranges in the PE exception directory
  (`.pdata`).
- **String anchors.** Unreal keeps reflection type names as literal strings,
  such as `ByteProperty`, `IntProperty`, `ObjectProperty` and
  `SpawnActor failed.`. [`tools/engine_discovery.py`](../tools/engine_discovery.py)
  finds these strings in data sections and lists the code that references them
  through RIP-relative operands. That narrows the search to the engine's
  core-UObject and actor-spawning code. See
  [PE-INSPECTION.md](PE-INSPECTION.md).

### 2.2 Using public knowledge of Unreal Engine 4

UE4's source code is public. Its core data structures (`UObject`, `UStruct`,
`UFunction`, `UProperty`, `FName`, `FUObjectArray`) and the logic of
`ProcessEvent` are well known. A community project for this game,
[DaysGoneUnrealModLoader](https://github.com/Mythical-Github/DaysGoneUnrealModLoader),
includes an SDK dump from an **older** build. Its addresses do not apply to the
current build, but it was used as a reference for:

- which classes exist (`BendPlayerPawn < BendPawn < Character`,
  `BendPlayerController`, `VehiclePawn`, …);
- which UFunctions exist and their parameter names (`GameplayStatics.BeginSpawningActorFromClass`,
  `K2_SetActorLocationAndRotation`, `SetMasterPoseComponent`, …);
- approximate structure layouts to confirm against the disassembly.

No code or assets from that project are included in this mod.

### 2.3 What was located in the binary

| Item | RVA / offset | How it was identified |
|---|---|---|
| `UObject::ProcessEvent` | `0x1AC2A20` | Behaviour in the disassembly matches UE 4.10's `ProcessEvent`: it tests `UFunction::FunctionFlags` (+0x88) for `FUNC_Native` (0x400), reads the script bytecode count (+0x50), and makes virtual calls at vtable +0x1E8 / +0x1F0 (`GetFunctionCallspace` / `CallRemoteFunction`). Its 19-byte prologue is position-independent, so it is safe to relocate into a trampoline. |
| `GUObjectArray` | `0x4C12290` | Global used by object iteration. Layout: `Objects*` at +0x10, `Max` +0x18, `Num` +0x1C; each `FUObjectItem` is 16 bytes, with flags at +8 and serial number at +0xC. |
| `GNames` (name table) | pointer found at runtime | Scanned at runtime, see §3.2. |
| `FNameEntry` string offset | **+0x12** | Found by disassembling FName initialisation (`0x19F86E0`). Stock UE 4.10 stores the string at +0x10, but Bend added a 16-bit field. The first in-game run failed with `GNames not found` until this was corrected. |
| `UProperty::Offset_Internal` | detected at runtime (+0x50) | See §3.3. |

Structure offsets confirmed for this build (also in
[`src/coop/ue.hpp`](../src/coop/ue.hpp)):

| Structure | Field | Offset |
|---|---|---|
| `UObject` | vtable / ObjectFlags / InternalIndex / Class / Name / Outer | 0x00 / 0x08 / 0x0C / 0x10 / 0x18 / 0x20 |
| `UField` | Next | 0x28 |
| `UStruct` | SuperStruct / Children | 0x30 / 0x38 |
| `UFunction` | FunctionFlags / ParmsSize (u16) | 0x88 / 0x90 |
| `UProperty` | ArrayDim / ElementSize / Offset_Internal | 0x30 / 0x34 / 0x50 |
| `FNameEntry` | Index (bit 0 = wide) / HashNext / string | 0x00 / 0x08 / 0x12 |
| `TNameEntryArray` | 128 chunk pointers × 16384 entries, NumElements | chunks at 0x000, count at 0x400 |

### 2.4 Runtime discovery inside the game

Some things only exist at runtime: which components make up Deacon, which
animation assets are loaded, and so on. With `Discovery=1` in the INI, the
plugin logs the local pawn's class chain and every component it owns
(skeletal meshes, anim blueprints, master-pose links). It also writes every
loaded `AnimSequence` to `DaysGoneCoop-anims.txt`. From these lists:

- the player pawn is `BendDefaultPlayerPawn_C`, controlled by
  `BendDefaultPlayerController_C`;
- Deacon is **modular**: a head mesh (`deacon_head_*`) plus 7–8 extra skinned
  parts (jacket, jeans, hat, forearms, bandoleer, bag, rings, …), all slaved to
  the main mesh's pose;
- usable locomotion clips are `dcn_idle_normal`, `dcn_walk`,
  `/Game/Animation/Deacon/run/run.Run` and `/Game/Animation/Deacon/run/sprint.Sprint`.

### 2.5 Engine quirks found by testing

- Bend's `SetSkeletalMesh` has **no** `bReinitPose` parameter, unlike stock
  UE4. The plugin's parameter checks caught this: the call was rejected and
  logged instead of writing outside the parameter buffer.
- `WasRecentlyRendered` does not exist on the pawn class in this build.
- Copying meshes alone gave a grey, untextured avatar. The materials have to
  be copied too (`GetNumMaterials` → `GetMaterial` → `SetMaterial`).
- Loading a save swaps Deacon's meshes and materials (for example
  `deacon_head_standard` → `deacon_head_scrag_beard`) and plays a fade-in. An
  avatar copied during that phase never becomes visible. The plugin now
  detects the change (different mesh, or the player moved more than 50 m),
  waits 5 s, then rebuilds the avatar.
- Under Proton, the ASI loader only loads if `version` is overridden to
  `native,builtin` for `DaysGone.exe` in the Wine prefix registry.

## 3. How the plugin works

Source layout:

| File | Role |
|---|---|
| [`src/coop/dllmain.cpp`](../src/coop/dllmain.cpp) | `DllMain` / exported `InitializeASI`; finds the plugin's `.ini`, `.log` and `-anims.txt` paths |
| [`src/coop/coop.cpp`](../src/coop/coop.cpp) | Build check, hook, tick loop, player tracking, avatar, teleport, config |
| [`src/coop/ue.hpp`](../src/coop/ue.hpp) / [`ue.cpp`](../src/coop/ue.cpp) | Minimal UE 4.10 reflection layer |
| [`src/coop/protocol.hpp`](../src/coop/protocol.hpp) | 144-byte v5 state packet and validation |
| [`src/coop/pose.hpp`](../src/coop/pose.hpp) | Compressed skeleton datagrams (`DPOS`) for body, bike and enemies |
| [`src/coop/skeleton.hpp`](../src/coop/skeleton.hpp) / [`skeleton.cpp`](../src/coop/skeleton.cpp) | Reads evaluated bone arrays, writes poseable copies, creates runtime components |
| [`src/coop/enemy.hpp`](../src/coop/enemy.hpp) | Enemy appearance descriptors (`DMEN`) and replica limits |
| [`src/coop/gear.hpp`](../src/coop/gear.hpp) | Held-item descriptors (`DMGR`) |
| [`src/coop/debug_commands.hpp`](../src/coop/debug_commands.hpp) | Allowlisted opt-in debug commands |
| [`src/coop/combat.hpp`](../src/coop/combat.hpp) | Consent-scoped reliable damage queue |
| [`src/coop/appearance.cpp`](../src/coop/appearance.cpp) | Private garment materials and texture tinting |
| [`src/coop/menu.cpp`](../src/coop/menu.cpp) / [`overlay.cpp`](../src/coop/overlay.cpp) | Settings input and Direct3D 11 overlay |
| [`src/coop/net.hpp`](../src/coop/net.hpp) / [`net.cpp`](../src/coop/net.cpp) | Non-blocking UDP peer (Winsock) |
| [`src/coop/log.hpp`](../src/coop/log.hpp) / [`log.cpp`](../src/coop/log.cpp) | Timestamped log file |

### 3.1 The ProcessEvent hook

`ProcessEvent` is the function through which Unreal runs every Blueprint
event and every reflected UFunction call. It runs many times per frame on the
game thread, which makes it a good place to run per-frame mod code.

`InstallHook` in [`coop.cpp`](../src/coop/coop.cpp):

1. Compares the 19 bytes at `base + 0x1AC2A20` with the expected prologue
   `40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00`
   (`push rbp/rsi/rdi/r12–r15; sub rsp, 0xF0`). If they differ, the plugin
   stops and leaves the game untouched.
2. Allocates an executable trampoline containing those 19 original bytes,
   followed by an absolute `jmp [rip+0]` back to `ProcessEvent + 19`. Calling
   the trampoline runs the original function.
3. Overwrites the original prologue with `jmp [rip+0] → HookedProcessEvent`
   and pads it with NOPs (`VirtualProtect` + `FlushInstructionCache`).

`HookedProcessEvent` records the thread of its first caller as the game
thread. On that thread it runs `Tick()` at most every 1/60 s, with a
re-entrancy guard, because `Tick()` itself calls `ProcessEvent`. It then always
forwards to the original. The mod never blocks: networking is non-blocking
and every loop is bounded.

### 3.2 Finding GNames at runtime

Unreal stores object and property names as indices into a global table,
GNames. Instead of hard-coding its address, `InitNames` scans the exe's
`.data` section. For each 8-byte slot it tries the slot itself, and also the
pointer stored in it, as a `TNameEntryArray`. A candidate is accepted when:

- entry 0 is `"None"` and entry 1 is `"ByteProperty"` (the first two names
  UE always registers);
- the element count is plausible (1000 to 2,097,152).

Every pointer is checked with `VirtualQuery` before it is read. On this build
the result is `GNames via pointer at rva 0x4aa22f8, 172315 names`.

### 3.3 Finding the property offset field

To read a property or fill a function parameter, the plugin needs
`UProperty::Offset_Internal`. Bend moved it, so `DetectPropertyOffsetField` takes a
function with a known layout, `GameplayStatics.GetPlayerPawn(WorldContextObject,
PlayerIndex) → ReturnValue`, whose parameters must sit at offsets 0, 8 and
0x10. It tries each candidate field offset from 0x40 to 0x74 and keeps the one
where all three parameters give those values. On this build that is **+0x50**.

### 3.4 Reflection layer

With GNames, GUObjectArray and the offset field known, the
[`ue`](../src/coop/ue.hpp) namespace provides:

- `FindClass`, `FindObject` and `FindDefaultObject` by iterating GUObjectArray
  and comparing name indices;
- `FindChild` and `FindFunction`, which walk `UStruct::Children` /
  `UField::Next` up the `SuperStruct` chain;
- `IsA`, `IsLive` (the object must be readable, its array slot must point back
  to it, and it must not be PendingKill or Unreachable) and `ResolveWeak` for
  `TWeakObjectPtr`;
- `Call`, which sizes a parameter buffer from `UFunction::ParmsSize`, writes
  each named parameter at its reflected offset (checking that it fits), then
  runs the original `ProcessEvent` through the trampoline. A missing function
  or parameter rejects the call and is logged once, instead of corrupting
  memory.

Example from the avatar code:

```cpp
Call move(avatar, "K2_SetActorLocationAndRotation");
move.set("NewLocation", pos).set("NewRotation", FRotator{0, yaw, 0})
    .setBool("bSweep", false).setBool("bTeleport", true).invoke();
```

Because it uses reflection, the mod calls engine functionality by **name**.
Apart from ProcessEvent and GUObjectArray, it has no hard-coded function
addresses.

### 3.5 Per-frame logic

`Tick()`:

1. **Engine resolution (once).** Finds GNames and the property offset, then
   `PlayerController`, `Character`, `SkeletalMeshActor`,
   `SkeletalMeshComponent`, `AnimSequence` and the `GameplayStatics` default
   object, then opens the UDP socket.
2. **Local player.** A cached `ULocalPlayer` supplies the active controller,
   with a periodic object-array discovery fallback. The controller and pawn
   must point back at each other; stale controllers from unloaded worlds can
   linger until garbage collection. A pawn change destroys the avatar.
3. **Send (30 Hz).** Reads the pawn's mesh-component location and rotation
   (actor location if it is not a Character), plus `GetVelocity`, and adds
   `GetWorldOriginLocation` so positions are world-absolute. Accumulated send
   deadlines avoid losing cadence to frame quantization; missed deadlines are
   skipped rather than emitted as stale bursts.
4. **Receive.** Drains up to 64 packets. Sequence numbers are compared with
   wraparound. After a 5 s gap the peer counts as reconnected, and the
   snapshot buffer and avatar are reset.
5. **Interpolation.** Sender timestamps preserve sample spacing even when the
   receiver drains several packets in one frame. The adaptive buffer starts at
   100 ms and stays within 80-250 ms. See the motion details below.
6. **Avatar.** See §3.6.
7. **Teleport.** If F7 goes from up to down while the game window has focus,
   the local pawn is moved to 150 cm beside the partner
   (`K2_SetActorLocation`, `bTeleport=true`).

Visual updates follow the local HUD's `ReceiveDrawHUD` event; a timed fallback
keeps initialization/networking progressing during loading or without a HUD.
The HUD name is resolved only after engine/name-table initialization. Looking it
up earlier caused a startup crash during testing and was corrected.

#### Motion timeline and profiling

[`motion.hpp`](../src/coop/motion.hpp) is independent of the engine calls.
It keeps up to 96 snapshots, unwraps the sender's 32-bit millisecond clock and
rejects backward timestamps. It estimates sender spacing and delivery jitter,
increasing buffer delay faster than it decreases it. A playback clock slews
within 0.98-1.02 times real time rather than snapping to every arriving packet;
errors over 0.5 seconds rebase it.

Positions use monotone cubic Hermite interpolation. Bike velocity comes from
source-time position differences because the mounted native pawn can report
zero velocity. Tangents do not cross on-foot/mounted/passenger transitions, and
per-axis limiting prevents position overshoot. Angles take the shortest path.
Both player and bike extrapolate for at most 150 ms, then hold; velocity is
bounded to 7000 cm/s. Unchanged positions suppress stale native velocity.
Teleports over 30 m and sender gaps over two seconds reset the timeline.

The remaining passenger jitter was traced to the clock feeding that timeline.
Packet send stamps, arrival times and render sampling all used
`QueryPerformanceCounter` at HUD-draw time. That point moves by several
milliseconds within a frame, so at motorcycle speed both the sender's samples and
the receiver's playback position wobbled by centimetres. [`frameclock.hpp`](../src/coop/frameclock.hpp)
now advances with the per-frame delta of `GameplayStatics.GetRealTimeSeconds`.
That engine value is fixed at frame start and ignores time dilation. The clock
slews 5% toward wall time, resyncs if the error exceeds 0.1 s, and falls back to
wall time for missing engine time, a non-positive or 0.25 s+ delta, or a world
change. It never runs backward. The `perf:` line reports its resync and fallback
counts. Unit tests drive 60 Hz frames with 0-8 ms HUD noise: frame steps
stay within 0.5 ms, whereas raw wall-clock steps vary by more than 5 ms.

Profiling first found repeated global object scans dominating the game-thread
budget. `FindObject`/`FindClass` now cache weak lookups, validating authoritative
object-array slot, pointer, serial, flags and identity metadata before reuse.
Missing objects retry after one second. Owned skeletal-component lists and the
local-player route are cached too. These caches do not root objects; material
ownership described in §3.9 remains separate.

Five-second `perf:` summaries report mod Tick cost, update/send/receive cadence,
riding lookup cost and motion-buffer counters. On cachy the measured baseline
was 9.45-9.93 ms mean / 122-129 ms peak, with only about 18 sends/s. After also
backing off absent-controller polling, the final riding check measured
0.67-0.73 ms mean / 5.6-7.2 ms peak on cachy and 1.77-1.96 ms mean /
18.9-23.7 ms peak on the Deck, with approximately 30 sends/receives per second
on each. Cached riding lookups
were below the 0.01 ms logging resolution instead of about 1.9 ms per call.
These measure plugin work, not total frame time or whole-game FPS. Cumulative
extrapolation/hold counters include startup; compare consecutive steady periods.

### 3.6 The partner avatar

There is no second "real" player character. The partner is a visual stand-in
built from engine primitives:

1. `GameplayStatics.BeginSpawningActorFromClass(SkeletalMeshActor)` +
   `FinishSpawningActor`, then collision is turned off.
2. `SetSkeletalMesh` copies the local player's main mesh, and each material
   slot is copied.
3. For each other skinned component owned by the local pawn (clothes, hat,
   accessories), a separate `SkeletalMeshActor` gets that mesh and its
   materials. It is attached to the avatar with `K2_AttachRootComponentToActor`
   (snap to target) and slaved with `SetMasterPoseComponent`, so it follows
   the body's skeleton.
4. Each frame the avatar is moved with `K2_SetActorLocationAndRotation`.
   `PlayAnimation` picks a looping clip by horizontal speed: < 30 cm/s idle,
   < 250 walk, < 520 run, otherwise sprint.
   The packet carries native aiming/crouch/reload state, a
   coarse weapon category and a monotonic shot sequence. At idle, aiming and
   crouching select locally loaded Deacon rifle/crouch sequences; a new
   gun/crossbow shot plays a short firing animation before returning to the
   locomotion pose. This first presentation slice does not clone the weapon
   model or reproduce muzzle flash/projectile effects.
5. The avatar is hidden (`SetActorHiddenInGame`) when the peer times out.

The partner therefore looks like *your* Deacon in *your* current outfit.

Speed-picked clips were later replaced by streaming the partner's real skeleton
(§3.13). The clip logic above is kept only as the fallback when no pose arrives.

### 3.7 Network protocol

[`protocol.hpp`](../src/coop/protocol.hpp) defines one fixed 144-byte,
little-endian UDP packet:

| Field | Type | Notes |
|---|---|---|
| magic | u32 | `"DGCO"` (`0x4F434744`) |
| version | u16 | 5; both clients must upgrade together |
| flags | u16 | HasPawn, InVehicle, HasBike, Passenger, FriendlyFire, AcceptPassengers, Alive, SyncBikes, WorldState |
| seq | u32 | wraps; compared with serial-number arithmetic |
| timeMs | u32 | sender clock |
| pos[3], yaw, pitch, vel[3] | f32 | must be finite and within ±1e8 |
| name | char[16] | player name |
| session, targetSession, consent, targetConsent | u32 each | connection/life/permission scope |
| attackId, attackAck | u32 each | retransmission and deduplication |
| damage | f32 | validated configured weapon damage |
| bikePos[3], bikeRotation[3] | f32 | position and pitch/yaw/roll |
| jacket, trousers, capColor, cap | u8 each | palettes 0-6 and cap visibility |
| attackKind | u32 | none, melee or gunshot |
| health | f32 | native health for state/consent checks |
| actions | u8 | aiming, crouched and reloading state bits |
| weaponType | u8 | none, gun, crossbow or melee |
| shotSequence | u32 | increments on local fire events for a remote firing pose |
| worldTimeOfDay, worldRelativeTime | f32 | host world clock values, valid with WorldState |
| worldWeather, worldTransitionRate | u8 each | host weather enum and transition mode, valid with WorldState |

Only the configured host sends WorldState. The joining client applies it every
0.5 seconds when neither world is in a mission or cinematic. It captures the
client's original time/weather and restores them after timeout or when the host
stops sending WorldState. These calls change the current game session only;
they do not write save data. In a two-game test the Deck time was corrected
from 10.651 to the host's 10.232, and a later host timeout logged restoration
from 10.724 to the captured local 10.241.

Weather needed a fix. The native `GetWorldWeather()` getter returned 0 even right
after a successful `SelectWorldWeather`, while the `WorldWeather` byte property
showed the new value. Earlier builds therefore always sent weather 0. The
snapshot now reads the property. With the opt-in `weather-next` command the
Deck followed the host 0 -> 1 -> 2 within 0.5 s, and `time-shift` moved the Deck
from 10.789 to 16.827 within a second. The client's own clock did not advance
between corrections, so corrections are now applied above 0.02 h (72 game
seconds) instead of 0.1 h to avoid visible lighting steps.

Version 5 also sends three further datagram types on the same socket, each
recognised by its own magic: `DPOS` skeleton poses, `DMEN` enemy appearance
and `DMGR` held items (§3.13). The state packet stays fixed at 144 bytes.

Peering is symmetric. In **host** mode (`PeerHost` empty) the plugin learns
the partner from the first valid packet. In **join** mode it sends to the
configured host. Either way it then pins the peer's IP and port and ignores
everyone else until the game restarts.

### 3.8 Motorcycle and passenger discoveries

The assigned bike is not the player pawn. `CurrentPlayerVehicle` on the game
mode is a **class**, not a vehicle instance: resolving it requires
`GameMode.GetVehicle(VehicleClass)`. While mounted, `Pawn.GetVehicle` and
`IsInVehicle` expose the actual vehicle. The proxy clones the chassis and 13
modular skeletal parts, preserves their materials and slaves their poses to the
chassis. Its transform is interpolated from the peer; its physics are not shared.
The chassis and rider control assets are held at their neutral pose rather than
looped as idle animations.

The native `SeatsComponent.SeatSetups` array describes driver/passenger sockets.
`SOCKET_PassengerSeat` belongs to a seat-part component rather than necessarily
the chassis. Passenger mode poses an independent modular body with
`bike_basic_control_buddy`, aligns its `hips` bone with that socket, and moves
the hidden native character kinematically for the camera and health system.
Fixed capsule-height offsets were rejected after screenshots showed the rider
floating above or sinking into the seat. Measurements also found that Bend's
native animation controller continued to change the local character's pose
despite single-node animation calls. Reusing the independent avatar factory
avoids that conflict. Bone offsets are composed in component space, avoiding
feedback from cached world transforms after a teleport. Packets describe the
independent body's mesh origin, matching the observer's identical riding pose.
For this body's root mesh, component space already is actor space: applying its
`GetRelativeTransform` again would incorrectly add the actor's world position.
After correcting that distinction, a stationary 30-second measurement held the
hip offset at 64.2 cm and visual root at -9989.2 cm, with seat variation below
0.2 cm, rather than the previous large oscillations.

The local passenger follows the interpolated remote bike. On the driver's
machine, however, interpolating the returned passenger world position adds a
second network delay and makes the body trail the native bike. The observer now
uses its own native bike's passenger socket and the independent avatar's
component-space hips offset. If the socket is unavailable, the body is hidden
and an explicit settings/log error replaces a misleading stale seat. This is
local visual anchoring, not authoritative shared vehicle physics.

Before boarding, the plugin captures movement/custom mode, native component
visibility, collision and the existing blueprint save-block flag. Each successful
state change is tracked separately, so a partial boarding failure does not undo
an input block or save block that the mod never acquired. Native animation mode
is left untouched. Passenger-originated weapon attacks are not supported.
It restores these on exit, driver dismount, permission removal or disconnect.
Ground and capsule traces check manual exits. A forced cleanup may fall back to
the original boarding position. The save block is an engine-wide boolean, not an
ownership token; concurrent native changes remain a limitation.

### 3.9 Garment colors and Unreal object lifetime

The Deacon parts use runtime material instances that wrap asset materials.
Following the `Parent` chain identifies jacket, jeans and cap material slots.
Setting `Fabric Tint 1` did not visibly recolor these shaders. Their actual
diffuse parameter is `Base Color (C/O) Map`. A private 1024-square
`CanvasRenderTarget2D` draws that source texture multiplied by the chosen palette
using `ReceiveUpdate` and `Canvas.K2_DrawTexture`. Normal maps and separate decals
remain intact.

`MeshComponent.CreateDynamicMaterialInstance` may return an existing dynamic
instance instead of isolating it. Mutating that instance leaked local red into
the peer's Original appearance. The replacement uses `KismetMaterialLibrary` to
create a fresh MID from the non-dynamic parent, copies source parameters and
explicitly assigns it.

A C++ pointer is **not a garbage-collector reference**. Once a private MID replaces
a native MID, UE may collect the original even though the plugin still remembers
its address. A crash dump located a failure during later material copying.
Originals now have shared native ownership backed by the supported build's
FUObjectItem RootSet flag. The last owner removes only a root added by the
plugin; preexisting engine roots are preserved. This allows Original restoration
and peer cloning without retaining every historical material indefinitely.

### 3.10 Settings renderer and threading

Initial attempts to draw through Canvas/HUD callbacks did not render reliably.
The current overlay uses pinned, MIT-licensed Dear ImGui v1.91.9b and the DX11
backend. A setup thread creates a dummy D3D11 swap chain to locate shared
`Present` and `ResizeBuffers` vtable slots. Those slots are replaced; this adds
no second machine-code detour. The renderer restores D3D state and releases its
render target on resize. Full swap-chain/device replacement is not established.

The game thread publishes an immutable frame through a mutex; Present owns the
ImGui context. Settings input supports keyboard, mouse and XInput, temporarily
suppresses gameplay input, and restores it on closing or losing focus. Six
preferences persist to INI; lethal friendly fire never does.

All four slots in `xinput1_3`, `xinput1_4` and `xinput9_1_0` are checked. When
none is connected, polling backs off to 250 ms. The tested Deck's official
layout used native Steam Input actions, exposing no XInput device. Switching to
Valve's Gamepad With Joystick Trackpad template exposed slot zero through all
three providers (`mask=0x111`). No native Steam Input action backend was added.
The user then confirmed menu opening/navigation and normal movement/driving
after closing the menu; no View/Back native-map interference was reported.

ProcessEvent also runs on other engine threads. An early reentrancy-guard write
from those threads could leave gameplay ticking stuck. Thread selection is now
atomic and other threads forward directly without touching the gameplay guard.
Nested render-target callbacks preserve its previous value.

### 3.11 Experimental friendly fire

`OnWeaponFired_Event` was observed on the local BendPawn during real shooting.
Native melee notifies did not consistently pass through ProcessEvent, so melee
instead samples `m_bIsInMeleeAttack` and the weapon's native collision window.
An attack is generated once on a window's rising edge, not merely on a mouse
button press. Geometric range/aim checks and a native world trace reject misses
and occlusion before transmission.

Weapon damage is read through reflected nested `CurWeaponConfig` fields. The
midpoint of min/max is used; native skills, criticals, spread, durability and
falloff are not fully reproduced. The receiver calls Bend's `ApplyDamage`,
including its additional `DamagedBodyIndex` parameter, and logs native return
damage and health before/after. A real boot-knife swing sent one 20-point event;
the receiving game's health changed from 121.9 to 106.9 after native processing.
The native return value was zero despite the health decrease, so the return alone
is not reliable evidence of damage. Subsequent pistol events requested 39 points
and removed about 29.3 native health: one logged sequence was
121.9 -> 92.6 -> 63.4 -> 34.1 -> 4.9 -> -24.4. The user confirmed both firearm
health loss and death/reload. Negative native death health is normalized to
zero on the wire, rather than sending a packet the decoder must reject.
Aim/range checks use the rendered avatar position so the target corresponds
to what the shooter sees, not a newer uninterpolated network position.

The queue holds at most 64 events, repeats the head until acknowledged,
deduplicates and expires events after three seconds. Session IDs come from
`BCryptGenRandom`; consent epochs invalidate attacks across toggles, life changes,
timeouts and reconnects. Both peers must opt in. These are consent/reliability
checks, not cryptographic authentication.

### 3.12 Native debug and controlled AI tests

An opt-in, game-thread reflection report found a live `BendCheatManager` under
the local `BendDefaultPlayerController_C`. Its native `CheatManager` base exposes
`God`, `Summon`, `DamageTarget`, debug-camera and time-dilation functions.
The controller already owns the manager; no external trainer injection is needed.
The game also has a live `GameCheatMenuBP_C` and `GameCheatMenu` APIs including
`ToggleCheatMenu` and `GetWorldDebugMenu`. Their presence does not prove that the
shipping debug menu itself is usable.

The public [Days gone remastered - Mod Menu](https://www.nexusmods.com/daysgone/mods/798)
advertises swarmer/horde spawning and an AI spawner. Its reuse permissions are
restricted. No binary, source, or assets from it were copied or installed;
the implementation below uses this game's own reflected interfaces.

The AI investigation changed from name matching to inheritance from `BendAIPawn`.
It found 106 non-default AI pawn objects, including `SwarmerPawn_C` and
`HordeSwarmerPawn_C`. Several had origin/staging positions or no controller:
these include pooled actors, not 106 verified active enemies. Controller,
location, health, mesh and subsequent movement are required evidence of activity.

`AIBlueprintHelperLibrary.SpawnAIFromClass` has this reflected layout:

| Parameter | Offset | Type |
|---|---:|---|
| WorldContextObject | 0 | UObject pointer |
| PawnClass | 8 | UClass pointer |
| BehaviorTree | 16 | UObject pointer |
| Location | 24 | FVector |
| Rotation | 36 | FRotator |
| bNoCollisionFail | 48 | bool |
| ReturnValue | 56 | pawn pointer |

The runtime uses reflected parameter names, not hardcoded copies of these
offsets. It spawns the loaded `SwarmerPawn_C` about six meters ahead, after
ground and capsule-clearance traces, with collision failure enabled and no
explicit behavior-tree override. The Blueprint/controller initializes the
native AI. Returned pawn/controller objects are rooted for ownership tracking;
ambient objects are never adopted. The first test spawned a real controller
and behavior-tree component, moved 729.2 cm, and accepted native damage:
health fell from 160 to 156.25 for five base damage. `ApplyDamage` returned zero
despite the health reduction, so tests judge actual health, not that return value.
The brain's `IsRunning` accessor is not reflected in this build; `running=-1`
means unavailable, not a stopped or verified running tree.

Invulnerability uses native `God()` once to measure which single bit changes
in the pawn's `bCanBeDamaged` byte. The observed mask was `0x02`.
The implementation then explicitly sets/restores only that measured bit,
preserving every adjacent bitfield, even if the controller later changes pawn.
The original flag is retained separately. In the first protected damage probe,
player health remained 121.875 before/after five native base damage.
This checks the normal damage entry point, not every scripted death/grapple path.

The local test mailbox accepts only documented command names. It is polled on
the game thread every 0.5 seconds, disabled by default, skips requests already
present at startup, and is never populated from a UDP packet. An atomic
standard-library Python helper publishes requests and waits for matching
acknowledgements. There is no generic console/UFunction endpoint.

`begin` captures `IsSavingBlockedByBlueprint`, enables
`SetSavingBlockedByBlueprint(true)` and verifies the getter before mutations.
Passenger boarding and partner teleportation are excluded while a test session
owns this state. Native damage/saving state is restored on `end`, pawn changes,
death, riding, missions/cinematics, or five minutes of command inactivity.
Cleanup destroys only tracked actors/controllers and refuses a controller that
has acquired an unrelated pawn. A first live test exposed deferred actor
destruction: checking `IsLive` immediately after `K2_DestroyActor` was too early.
The corrected implementation requests all owned destructions, waits across engine
frames, and acknowledges cleanup only after both pawns/controllers are no longer
live. A ten-second failure reports an error and leaves saving blocked.

The final scripted pass on cachy (ASI SHA-256 `d524ca6e…ef12`) ran 20 requests,
all with the expected result. One swarmer spawned and took native damage
(200.000 → 196.250), then cleanup brought the owned count to 0. Two packs reached
16 owned enemies, and the 17th spawn request was rejected. With `god-off`, the
damage bit was restored (`flags 0x38 → 0x3a`) and the player took damage
(121.875 → 120.938). With `god-on`, health stayed at 120.938. Cleanup and `end`
restored the original damage and saving state. Save file hashes were unchanged
throughout the session. Spawned enemies were not replicated to the Deck; that is
still host-authoritative enemy work.

The real horde Blueprints expose `ActivateHorde`, `SetSize`, `AddHordeMembers`,
`ReloadHorde`, and `ForceDestroyHorde`, alongside `SaveDatabase` and completion
logic. These are not used for disposable tests because they can affect persistent
horde progress. `spawn-pack` instead creates eight independent native swarmers,
with a hard limit of 16 owned enemies. A real horde's crowd/group behavior and
host-authoritative enemy replication remain separate work.

The native save blocker is not full save isolation. Saves were backed up on cachy
before the encounter tests. Indirect gameplay/progression effects and ordinary
autosaves outside the test session remain possible; disposable saves are required.

### 3.13 Skeleton streaming, held items and enemy replicas

"Perfect mirroring" cannot come from guessing animations, because Days Gone
mixes hundreds of state-machine clips, additive layers and IK. Instead the
sender reads the **evaluated** bone array of a skinned mesh after the game has
animated it, and the receiver writes those transforms into a
`PoseableMeshComponent` copy:

- **Reading.** `PoseReader` takes the component-space bone transforms that the
  engine already computed for rendering and converts them to parent-relative
  transforms. Bones are identified by a hash of their name, not by index, so
  meshes with different bone orders still match.
- **Wire format.** A `DPOS` datagram has a 44-byte header (kind, session,
  sender time, entity, chunk, flags, root, yaw, health) and up to 80 bones of
  17 bytes each. Translations are 16-bit at 0.02 cm; rotations use
  "smallest-three" quaternion compression. A full body takes two or three chunks.
  Body and bike poses go out with each state packet (~30 Hz), enemies at 15 Hz.
- **Writing.** `PoseWriter` interpolates two received poses on the same
  sender-time timeline as movement (§3.5) and writes them into the poseable copy.
  Clothing, hair and accessories stay slaved with `SetMasterPoseComponent`, so
  they follow automatically.
- **Bike.** The partner's bike is rebuilt from its chassis plus its modular
  parts. Seven parts share the chassis skeleton and are slaved to it; six hang
  from named sockets on other parts. The chassis skeleton is streamed as a
  `kPoseBike` pose, so steering, fork compression and wheel spin come from the
  partner's real bike.

**Socket-follow fix.** Writing the bone array directly skips Unreal's normal
"update children" step. Anything attached to a *socket* on a posed mesh therefore
stayed where it was when the parent actor last moved, and on a standing avatar
it froze entirely. This hit both the held weapon and the six socket-mounted bike
parts (one cause of the "bike seems incomplete" report). Every frame,
`FollowSocket` now composes the live socket transform with the recorded offset
and places the child in world space. In the log, the held item then matched
`r_hand_socket` to within 1 cm.

**Held items (`DMGR`).** The sender asks the pawn for
`GetEquippedItemCurrentlyInHand`, takes the item's mesh asset, walks
`AttachParent` up to the character mesh to find the socket name, and records the
item's transform relative to that socket. It sends a 240-byte descriptor
whenever this changes and every 0.5 s otherwise. The receiver looks up the same
asset by path (`FindObject`, which never loads from disk), puts it on a
`SkeletalMeshActor`, or a runtime `StaticMeshComponent` for static items, and
attaches it to the avatar's socket.

Days Gone fades fresh meshes in through per-instance dynamic materials, so the
asset's default materials can stay invisible. The receiver therefore borrows the
materials of a live, visible component that shows the same asset, usually the
local player's own copy of that weapon. Finding it scans the object array (40-80 ms
the first time), so the donor is cached per asset. If the receiver doesn't
have the asset loaded, nothing is shown; that is logged once.

**Enemy replicas.** The host walks the object array in time-sliced batches to
find `BendAIPawn`s within 80 m of either player, at most 16. For each one it
streams the evaluated skeleton as `kPoseEnemy` (at most two chunks, carrying the
world root and health) plus a `DMEN` descriptor with up to four skeletal-mesh
asset paths (body and modular parts) every second. The client builds a poseable
replica the first time it sees an entity, keeps it in step with the stream, and
destroys it after 1.5 s of silence or when health reaches zero. The replicas
have no AI, collision or damage on the client, and the client's own enemies
still exist independently. Bandwidth is roughly 140 kB/s per eight enemies.

**Debug spawns.** The opt-in `spawn-single`/`spawn-pack` commands give a
repeatable enemy source for this. On the Deck, host-spawned swarmers appeared,
were posed, and expired once cleaned up on the host.

## 4. Safety measures

- Exact build check (timestamp + image size) and prologue byte check before
  any patching.
- All engine work happens on the game thread, inside the engine's own call
  path.
- Pointers are checked with `VirtualQuery`; objects are checked with
  `IsLive` before every use.
- Reflection calls check function and parameter names, sizes and offsets.
- Packets are strictly validated: size, magic, version, flags, finite
  values, sequence numbers.
- Spawn retries are rate-limited to one every 2 s; packet processing is
  bounded per frame.
- A `FATAL` log entry disables the mod's gameplay work; the game keeps
  running.

## 5. Testing

- **Unit tests** (`make test`): the actual C++ packet decoder
  ([`tools/test_protocol.cpp`](../tools/test_protocol.cpp)), and Python tests
  for the simulator, protocol, PE parsing and engine discovery.
- **Simulated partner**: [`tools/fake_peer.py`](../tools/fake_peer.py) speaks
  the protocol and plays a ghost that circles, stands still or follows the
  player. [`tools/same-pc/`](../tools/same-pc) wraps it for single-PC tests.
- **In-game checks on `cachy`**: the game was launched over SSH and driven
  with `xdotool` (keys must be held for about 0.4 s to register). Screenshots
  were taken with `spectacle` and compared with the plugin log. The status
  line records the avatar's world position, its projected screen position
  (`ProjectWorldLocationToScreen`) and component visibility. This separates
  "not rendered" from "rendered but off-camera"; that check found a 90° yaw
  convention bug in the simulator. It also records the remote action bits,
  weapon category, shot sequence and selected animation. With `Discovery=1`,
  local action/weapon changes are logged as `presentation:` lines.
- **LAN check**: a second machine exchanged packets with the game through the
  firewall, including the host-mode handshake and reconnect.
- **Two real games (2026-10-18)**: Days Gone on a Steam Deck joined cachy over
  the LAN using the same ASI under Proton. Both games loaded a progressed save.
  Moving either player changed the received position in the other game's log;
  cachy's movement also selected the walk animation on the Deck. A screenshot
  on cachy confirmed the Deck-controlled avatar rendered with textures. F7
  teleport was tested on the Deck through keyboard input. Current save folders
  were backed up before copying the test save, with the user's approval.

Expansion checks confirmed the DX11 settings overlay, red garments, cap hiding,
separate Original/Crimson and Crimson/Ocean appearances, native bike mounting,
the modular bike proxy, passenger boarding/following and safe dismount. Original
materials were restored after several minutes without the earlier collection
crash. The matching final ASI was deployed to both machines and its SHA-256
verified against the package
(`e1b69096b6fc98ce089a23763c1b43908b248dac41bbe726705d1c699880675c`).
Passenger alignment uses the live `hips` bone and
native passenger socket: the settled component-space hips remained at 64.2 cm,
with about 0.1 cm stationary visual-root variation. Short rolling/braking,
manual exit/reboarding, driver dismount and disconnect cleanup were checked.
The latter restored the native body, movement, collision and saving.

The current suite passes 44 Python tests plus native C++ tests for the v5 wire
format, pose/enemy/held-item datagram round trips and rejection rules, serial ordering, mutual consent, scope invalidation, retransmission and
deduplication, plus bone-to-socket alignment across yaw and translation changes.
Motion tests cover timestamp wrap, teleport/gap recovery, role transitions,
bounded bike/player extrapolation and paused stale velocity. A deterministic
nine-second simulation combines 30 Hz sending, 60 Hz rendering, batched arrivals,
jitter and one-in-seven packet loss. After warmup, constant-velocity frame steps
must remain within 97.9-102.1% of ideal and track playback time within 0.02 cm.
Native knife damage was observed (121.9 to 106.9 health for one transmitted
20-point event). A one-sided opt-in test reached the native melee collision
window but queued no damage while the partner remained off. Native pistol
damage/death logs and the user's death/reload confirmation are described in
§3.11. The final build passed a fresh short passenger drive/brake check, with
the seated body visible on both clients. The final controller layout exposed
XInput after restarting the game. The physical playtest confirmed Deck menu
and normal gameplay controls. Moving passenger behavior was described as
"much smoother, not perfect"; residual jitter remains and is not claimed fixed.
The later frame-locked clock build (SHA-256
`c8b257f8d2ce5c8ab93bdd4599f8764476fc289e528ecd069fafdc1521f6e6fb`) was
deployed to both machines. Its engine clock stayed locked through a
drive/brake check with the seated passenger visible on both clients. Clock
resync/fallback counts did not change while riding. The check measured about
0.7-0.9 ms mean mod Tick on cachy and 1.4-1.9 ms on the Deck, with about
30 sends/receives per second. Its perceived smoothness still needs a physical
playtest.
Long-distance travel, native Windows and long-session stability remain unverified.
Movement and visual motorcycle replication do not establish shared-world gameplay.

The Snyk code-scan tool was unavailable in this environment; no Snyk scan was
performed. Unit tests and live gameplay checks are not a security assessment.
Live checks for the v5 build (2026-10-19, both machines on the same build):

- **Pose mirroring.** The streamed body showed the partner's real stride and
  strafe and a crossbow/rifle aim pose on the other screen.
- **Held weapon.** The weapon rendered in the avatar's hands while aiming.
  Firing itself (projectile, muzzle flash) is not replicated.
- **Enemies.** Host enemies were replicated and expired on the Deck.
- **Time and weather.** Time and weather values followed the host, as described
  in §3.7.
- **Bike.** The rebuilt bike rendered complete. The socket-mounted parts' new
  per-frame follow is the code path validated for held items, but it has not
  been visually re-checked while riding.
- **Not yet checked.** Mission/cinematic pausing of world sync has not been
  exercised live.

Further runtime reflection identified the open-world `BP_BendWorldManagerV3_C`
instance and its native `BendWorldManager` base. The base exposes `WorldTime`,
`WorldWeather`, mission/cinematic state, `SetWorldTimeOfDay(float, bool)` and
`SelectWorldWeather(byte, byte, float)`. The host sends these values;
the joining client applies them only outside mission/cinematic state and
restores its prior session values after disconnect. See §3.7 for the
`GetWorldWeather()` quirk. The same scan found `BendFreakerPawn`,
`BendAIPawn` and `BendFreakerController` classes, including health/death and
awareness surfaces. The original name-only scan did not establish whether active
freakers were present; the later inheritance scan found pooled and controlled AI
objects, and controlled tests successfully spawned native swarmers. Those spawns
are now used to exercise the visual enemy replicas in §3.13.

## 6. Limitations and next steps

This is a **ghost co-op prototype with experimental gameplay extensions**. Each
player runs their own single-player world. Full shared-world co-op would
also need:

- authoritative enemies: the replicas in §3.13 are visual, so the client cannot
  fight the host's enemies and still has its own;
- projectile/muzzle-flash replication and native damage parity;
- authoritative motorcycle physics, upgrades, fuel and damage;
- syncing missions and saves, plus world state beyond session time/weather;
- reducing pose bandwidth (delta or keyframe compression) before any non-LAN use.

The shipped single-player gameplay does not provide a usable campaign co-op
mode. Shared-world sync must use the game's gameplay APIs and respect mission
state; the existence of Unreal networking APIs is not enough.
