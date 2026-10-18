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

The game's files on disk are never modified. Removing the `.asi` file
removes the mod.

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
              ├─ on game thread, 60 Hz: Tick()    │
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
| [`src/coop/protocol.hpp`](../src/coop/protocol.hpp) | 64-byte wire format and validation |
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
2. **Local player.** Once per second it looks for the newest live
   `PlayerController` whose `Pawn` points back at it; stale controllers from
   unloaded worlds can linger until garbage collection. A pawn change destroys
   the avatar.
3. **Send (30 Hz).** Reads the pawn's mesh-component location and rotation
   (actor location if it is not a Character), plus `GetVelocity`, and adds
   `GetWorldOriginLocation` so positions are world-absolute.
4. **Receive.** Drains up to 64 packets. Sequence numbers are compared with
   wraparound. After a 5 s gap the peer counts as reconnected, and the
   snapshot buffer and avatar are reset.
5. **Interpolation.** The partner is rendered 100 ms in the past, interpolated
   between snapshots, with up to 250 ms of velocity extrapolation.
6. **Avatar.** See §3.6.
7. **Teleport.** If F7 goes from up to down while the game window has focus,
   the local pawn is moved to 150 cm beside the partner
   (`K2_SetActorLocation`, `bTeleport=true`).

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
5. The avatar is hidden (`SetActorHiddenInGame`) when the peer times out.

The partner therefore looks like *your* Deacon in *your* current outfit.

### 3.7 Network protocol

[`protocol.hpp`](../src/coop/protocol.hpp) defines one fixed 64-byte,
little-endian UDP packet:

| Field | Type | Notes |
|---|---|---|
| magic | u32 | `"DGCO"` (`0x4F434744`) |
| version | u16 | 1 |
| flags | u16 | bit 0 HasPawn, bit 1 InVehicle; other bits are rejected |
| seq | u32 | wraps; compared with serial-number arithmetic |
| timeMs | u32 | sender clock |
| pos[3], yaw, pitch, vel[3] | f32 | must be finite and within ±1e8 |
| name | char[16] | player name |

Peering is symmetric. In **host** mode (`PeerHost` empty) the plugin learns
the partner from the first valid packet. In **join** mode it sends to the
configured host. Either way it then pins the peer's IP and port and ignores
everyone else until the game restarts.

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
  convention bug in the simulator.
- **LAN check**: a second machine exchanged packets with the game through the
  firewall, including the host-mode handshake and reconnect.
- **Two real games (2026-10-18)**: Days Gone on a Steam Deck joined cachy over
  the LAN using the same ASI under Proton. Both games loaded a progressed save.
  Moving either player changed the received position in the other game's log;
  cachy's movement also selected the walk animation on the Deck. A screenshot
  on cachy confirmed the Deck-controlled avatar rendered with textures. F7
  teleport was tested on the Deck through keyboard input. Current save folders
  were backed up before copying the test save, with the user's approval.

Not verified: shared combat, riding the bike, long-distance travel, native
Windows and long-session stability. Two real instances exchanging movement
does not establish shared-world gameplay.

## 6. Limitations and next steps

This is a **ghost co-op prototype**. Each player runs their own single-player
world, and only player positions and movement are shared. Real co-op would
also need:

- syncing enemies (freakers, NPCs) with one authoritative host;
- combat, damage and death syncing;
- vehicles: the bike as a replicated actor with rider animation;
- syncing world state, missions and saves;
- matching animations to the partner's actual state instead of speed tiers.

Days Gone ships with no networking code for any of this, because the engine's
replication layer is unused. Each item needs further reverse engineering
of Bend's gameplay classes.
