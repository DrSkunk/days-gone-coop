# Offline PE inspection

**Offline investigation only, not runtime engine validation.** The test
fixtures distinguish code from nonexecutable data, build IDs are restricted to
ASCII digits, and tests cover malformed structures, translation boundaries,
fingerprint failures, read-only CLI behavior and metadata-change detection.
No fixture test establishes that an engine address or layout is safe.

Run only with explicit caller-selected files:

```sh
poetry run python tools/pe_image.py --exe /path/to/image.exe \
  --manifest /path/to/appmanifest_1259420.acf \
  --inventory /path/to/installed-files.json
```

The utility reads the executable bytes, hashes them, and requires both that SHA-256 and the manifest's unique simple quoted `buildid` to match inventory fields `game_sha256` and `game_build`. It reports section metadata only after both matches. It accepts AMD64 PE32+ images, 1–96 sections, and validates header/directory/section bounds and non-overlap, including requiring `SizeOfHeaders <= SizeOfImage`. Inventory build IDs must be nonempty numeric strings and hashes must be 64-character hexadecimal strings. Manifest parsing supports only a unique simple quoted numeric `buildid` scalar; arbitrary VDF syntax is not interpreted. File reading checks regular-file identity and metadata before and after a bounded read; this is best-effort concurrent-change detection, not an atomic snapshot or protection against malicious races.

A file offset is a position in the on-disk file; an RVA is relative to the image's loaded base, independent of preferred `ImageBase` and ASLR. Headers translate linearly within `SizeOfHeaders`. Sections translate only within raw-backed correspondence: `min(SizeOfRawData, VirtualSize)`, or raw size when `VirtualSize` is zero. Thus virtual-only zero-fill, gaps, overlays, and raw alignment padding beyond a nonzero virtual size do not translate. Section occupancy validation uses `max(VirtualSize, SizeOfRawData)`. Executable characteristics are metadata, not proof of callable code. No rounded page-span mapping is claimed.

Output is offline metadata, not runtime compatibility or engine evidence. A matching caller-provided inventory/hash is not independent runtime verification. No offsets, signatures, functions, or layouts are inferred; no proprietary bytes are emitted. This implements standard PE-format interpretation only; no upstream tooling was downloaded or evaluated. It does not complete Gate0 or establish hooks, gameplay, or co-op.

## Engine-anchor candidates

```sh
make inspect EXE="/path/to/DaysGone.exe" \
  MANIFEST="/path/to/appmanifest_1259420.acf"
```

`engine_discovery.py` reuses the fingerprint gate and the same in-memory bytes
for parsing and discovery. It finds a small fixed set of null-terminated
reflection/spawning markers in raw-backed, nonexecutable section data, in
ASCII and aligned UTF-16LE. It reads function ranges from the AMD64 exception
directory, checks backed and executable extents, and uses Capstone to decode
RIP-relative memory operands from those function starts. Displacements are
signed and addresses are reported as RVAs, not preferred or live virtual
addresses.

The report deliberately calls the results `offline_candidates_only` and
sets `runtime_engine_access` to `false`. String references do not identify
GNames, GUObjectArray, ProcessEvent, a safe tick callback, actor spawning APIs
or game-specific property layouts. The tool does not attach to a process,
patch memory, write inputs or return executable bytes.

Disassembly stops at undecodable bytes within each function and reports the
number of incomplete functions and total decoded bytes. Exception metadata
does not cover every function, and even a decoded sequence may be embedded
data rather than a real instruction path. There is no claim of exhaustive
references or safe hook sites. Missing anchors, missing references, malformed
exception tables, fingerprint mismatches and exceeded result limits are
explicit errors with nonzero exit status and no JSON success output.
