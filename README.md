# NIF Slot Sniper 2.0.0

NIF Slot Sniper (NSS) is a GUI tool for Skyrim SE/AE that lets you view, edit, and bulk-synchronize
**biped armor slots** between `.nif` meshes and `.esp`/`.esl` records. It can also rewrite the slots of
original BodySlide `.nif` files.

Released under the MIT License (see [License](#license)).

> **Designed to run under Mod Organizer 2.** Launching through MO2 is required so that the virtual file
> system (USVFS) resolves the correct load order and loose textures.

---

## What's new in 2.0.0

2.0.0 is a major rewrite. If you are coming from 1.0.0, please read [Migrating from 1.0.0](#migrating-from-100).

- **Synthesis is no longer required.** The old `slotEXporter` / `slotIMporter` Synthesis patchers are
  replaced by a standalone CLI, `slottool.exe` (Mutagen / .NET 8), which the GUI runs for you. Like its
  predecessors, `slottool` is published as a **separate project** with its own repository and license.
- **Per-mesh slots.** Slots are now tracked per mesh (NIF partition) instead of as a single ARMA union,
  so multi-slot outfits export correctly.
- **JSON edit history.** `slotdata-Output.txt` is replaced by `slotdata-ChangeSet.json` (not compatible
  with the old text format — a one-click converter is provided).
- **Direct Overwrite mode**, **BSD ⇄ NiSkin conversion**, **texture rendering** (diffuse, alpha,
  normal maps, AlternateTextures, BSA support), and a translucency-aware **KID Generator**.

---

## Requirements

- Mod Organizer 2 (the tool reads the live load order through it).
- `slottool.exe` — the companion CLI, distributed separately
  (https://github.com/HeavyMoon-nexus/slottool). Place it anywhere and set its path under
  **File → Settings**.
- Optional: `BSArch.exe` (classic CLI build) to read textures packed inside `.bsa` archives. Set its path
  in Settings. Loose textures work without it.

---

## Quick start

1. **File → Settings**: set the **Game Data Path**, **Output Root**, **slottool path**, and (optionally)
   **BSArch path**. Leave defaults for the rest.
2. **NIF Database → Import DB (slottool)**: NSS runs `slottool export` against your live load order, then
   reads each NIF to build per-mesh slot data. A browseable database tree appears (ESP → gender → NIF).
3. Double-click a NIF to load it into the 3D viewport.
4. In the **Control Panel**, pick a mesh, set the new slot(s), and press **Apply**. The change accumulates
   in the **Item Pending Save** window.
5. In the **Pending** window, tick what you want to write (**NIF / ESL / slotdata Json Out**) and press
   **Export Pending**.

---

## Data flow

```
[live load order]
      | slottool export (child process, inherits USVFS)
      v
 nss_slot_export.json (%TEMP%)
      | Import DB: parse JSON -> records, then read each NIF for per-mesh slots + bones
      v
 in-memory database (browse tree; not persisted)
      | select a row -> load NIF -> 3D viewport
      | edit partitions -> Apply
      v
 Pending list (g_SessionChanges)
      | Export Pending (NIF / ESL / ChangeSet json)
      +-- NIF : write per-mesh partitions back into each shape
      +-- ESL : slottool import -> <source>_SlotPatch.esp  (or Direct Overwrite to the origin plugin)
      +-- json: update slotdata-ChangeSet.json (edit history / batch input)
```

Alternate paths:

- **Batch**: *Load slotdata → Pending* expands `slotdata-ChangeSet.json` into the Pending list so you can
  re-export everything at once (useful for applying a downloaded ChangeSet).
- **Convert**: *Convert old TXT → ChangeSet.json* migrates a legacy `slotdata-Output.txt` by reading the
  NIFs to reconstruct per-mesh slots.
- **OSP**: the **BodySlide OSP Browser** tab edits/exports BodySlide `.osp` source meshes directly.

---

## The windows

- **NIF Database** — the imported records as a tree (ESP / gender / NIF). Search filters support
  `esp:`, `slot:`, and `mesh:` prefixes (exact match). NIFs whose on-disk partitions do not match the
  ESP slots are highlighted orange after **Check NIF↔DB Slots**.
- **Control Panel** — gender priority, texture mode, load mode, NIF loading, **Analyze Slots**, the Apply
  buttons (This Mesh / per-partition / ALL), normal-map debug toggles, and **Load Ref Body**.
- **Item Pending Save** — the queued edits and the unified **Export Pending** controls.
- **Analysis Details** — explains *why* each slot was suggested (which rule added how many points).
- **Settings** — all paths, texture cache size, log level, reference-body texture folder, Direct Overwrite,
  and skeleton/costume-seed options.

**Load modes** (Control Panel): *Single* loads NIFs without `_0/_1`, *Pair* loads `_0/_1` body pairs,
*Both* loads either (default). *BS OSP NIF* mode reads BodySlide `.osp` (press **Scan OSP Files**;
not auto-loaded).

---

## Editing slots

- Select a mesh in the mesh list. Its current partition slots are shown per partition
  (`[i] slot N (name)`).
- Enter the new slot(s) manually, use the **per-partition editor** (one slot per partition), or take a
  suggestion from **Analyze Slots**.
- Press **Apply** to stage the change in **Item Pending Save**.
- The number of slots you can write to a mesh is limited by its partition count; exceeding it is rejected.

### Per-mesh model (why your block slots are preserved)

The ESP `armaSlots` (BodyTemplate) can contain "block" slots that have **no geometry** (used to claim a
slot so other gear can't occupy it). The NIF only has partitions for real geometry. NSS keeps these
separate: on import it computes `blockingSlots = armaSlots − union(mesh slots)` and, after you edit,
rebuilds `armaSlots = union(mesh slots) ∪ blockingSlots`. So editing a mesh's geometry slot never drops
your block slots.

The per-mesh truth lives in the NIF. Slots are read for the **currently displayed gender** only (half the
cost); to capture the other gender, switch gender and re-run **Import DB**.

---

## Export options

In the Pending window, **Export Pending** writes the ticked outputs:

- **NIF** — writes each shape's partition from the per-mesh data. Output goes under
  `Output Root/meshes/...`. Pair NIFs (`_0/_1`) are handled together.
- **ESL** — runs `slottool import` to produce `<source>_SlotPatch.esp`. If a patch already exists it is
  loaded and merged (existing FormKeys are updated, new ones added).
- **slotdata Json Out** — merges the edits into `slotdata-ChangeSet.json` (your portable edit history).

### Direct Overwrite mode

Enable **Direct Overwrite** in Settings to skip patch generation and write **into the original files**:

- The NIF is backed up once (`.bak`) and then overwritten in place.
- The ESP slot change is written directly to the **defining plugin** via `slottool overwrite-original`
  (also backed up to `.bak`). Records are routed to their origin plugin even if ARMA and ARMO live in
  different mods.

This is destructive (off by default). When it is on, the Export button turns red and is decorated with
`## DIRECT OVERWRITE ##`. The OSP path is not affected by this mode.

### BSD ⇄ NiSkin conversion

- **Convert to NiSkin (remove slots)** turns the selected `BSDismemberSkinInstance` mesh into a plain
  `NiSkinInstance`, removing it from the slot set; `armaSlots` is recomputed accordingly.
- **Convert to BSD (add slot)** does the reverse, giving a NiSkin mesh a partition (default slot 32) so you
  can assign slots to it.

Conversions are persisted in the ChangeSet and re-applied on export (and to the original NIF under Direct
Overwrite).

---

## Analyze Slots (slot suggestion)

`slottool export` usually returns vanilla slots, so setting each piece by hand is tedious. **Analyze Slots**
(Control Panel) proposes the **1st / 2nd / 3rd** most likely slots for the selected mesh using a scoring
system. Pick one, then **Apply** as usual. Open **Analysis Details** to see the score breakdown.

Rules (edit them in **Settings → Auto-Fix Rules**):

- **Name Rules** — award a score to a slot when a search word matches the mesh name or EditorID.
- **Bone Rules** — award a score when a search word matches one of the mesh's skin bones (physical
  position).
- **Combo Rules** — award a bonus only when *all* listed bones are present on the mesh (e.g. `L foot`,
  `R foot`). Most reliable.

A good ordering of weights is **Combo Rules > Name Rules > Bone Rules**.

---

## Texture display

Toggle **Texture Mode** to render diffuse + alpha + double-sided materials, plus normal maps and a light
specular term. Notes:

- Loose textures are read directly; textures inside `.bsa` are read via BSArch (indexed once, then
  unpacked on demand into a temp cache).
- **AlternateTextures** (color variants defined on the ESP record) override the baked NIF textures for the
  selected record. Re-run **Import DB** after upgrading so these fields are present.
- Body meshes use model-space normals (`_msn`) and are excluded from tangent-space normal mapping
  automatically; outfit normals (`_n`) get the effect.
- **Load Ref Body** shows a reference body so you can check overlap; turn on **Show Ref** to display it
  (hidden by default). Its textures can be resolved from a dedicated **Ref Body Texture Folder** in
  Settings.

---

## KID Generator

Generates `_KID.ini` files for the Keyword Item Distributor (Type is fixed to `Armor`).

- Type a keyword or pick one from the dropdown. Tick ARMOs in the NIF Database and **Add ARMOs to KID
  List**, then **Generate String** and **Append to output_kid.ini** (written under Output Root, appended
  if the file exists).
- **Keyword Auto Pickup**: NSS inspects the loaded mesh and surfaces keywords linked by slot/name at the
  top of the dropdown. `*` marks a slot or name match; `**` marks both.
- **Translucency suggestion**: if any mesh uses alpha blending, sheer/translucent keywords are sorted to
  the top and highlighted, with a hint in the header.

---

## Blocked lists

- Right-click an ESP in the NIF Database → **Add ESPs to BlockedList** to hide it and skip it from export
  (e.g. a male NIF that points to vanilla Skyrim).
- Right-click a mesh in the Control Panel's mesh list to add it to the Meshes BlockedList; it greys out and
  is excluded from Analyze (so collision/unwanted bones aren't picked up).
- Both lists are editable under **File → Settings**.

---

## Files produced

In the executable's folder:

- `imgui.ini` — window layout.
- `config.ini` — paths, custom slot names, blocked lists, and options.
- `keywords.json` — KID keyword list.
- `nss_log.txt` — session log (3-generation rotation; level set in Settings).
- `nss_permesh_cache.json` — per-mesh cache keyed by NIF path + mtime + size (delete to force a rebuild).

In your Output / data folders:

- Exported NIFs under `Output Root/meshes/...`.
- OSP-derived NIFs under `CalienteTools/Bodyslide/ShapeData/...`.
- `<source>_SlotPatch.esp` slot patches (unless using Direct Overwrite).
- `*_KID.ini` per ESP (appended if present).

In `slotDataPath` (default `slotdataTXT`):

- `slotdata-ChangeSet.json` — your edit history (also the batch input for *Load slotdata → Pending*).
- `costume_seed.json` — optional export for downstream tools (see below).

---

## Migrating from 1.0.0

- The 1.0.0 workflow (Synthesis `slotEXporter`/`slotIMporter` + `slotdata-Output.txt`) is gone. Build your
  database with **Import DB (slottool)** instead.
- `slotdata-Output.txt` is **not** read by 2.0.0. To keep old edits, use **Convert old TXT →
  ChangeSet.json** once; NSS reads the NIFs to reconstruct per-mesh slots (rows whose NIF can't be read
  fall back to the union only).
- The removed buttons **Export Selected NIF** and **Write slotdata-output.txt** are consolidated into the
  Pending window's unified **Export Pending**.

---

## Advanced: costume seed (skin-bone extraction)

For pipelines that attach skinned accessories to the skeleton without consuming a biped slot, NSS can emit
a `costume_seed.json` describing each record's skinned shapes, their bone names, and whether those bones
resolve against a target skeleton.

- Set **Skeleton Path (Female/Male)** and enable **Export costume seed** in Settings.
- The seed is written from the full database at the end of **Import DB / Convert** (independent of the
  ChangeSet, so it never pollutes your edit history). Only records with at least one skinned shape are
  included.
- Bone resolution is a static pre-filter for the displayed gender, not a runtime guarantee; consumers
  should re-check at load time. Each `id` is a stable Mutagen FormKey (`XXXXXX:Plugin.esp`) and should be
  treated as an opaque key.

---

## Roadmap

- Loading multiple NIFs as references for side-by-side comparison.
- Routing per-mesh data through the OSP export path.

---

## FAQ

**Q. `slottool import` reports "Lower FormKey range was violated" for some mods.**
A. That mod is flagged ESL/ESPFE but contains FormIDs below the legal small-master range. Direct Overwrite
handles this by preserving the original FormIDs. If you are generating patches and still hit it, open the
mod in SSEEdit (xEdit), right-click it, run **Compact FormIDs for ESL**, and save.

**Q. The viewport loaded a NIF but nothing appears.**
A. Likely the Meshes BlockedList matched a mesh name. The log warns when every mesh in a NIF is hidden by
the blocklist. Remove the matching word in Settings.

---

## License

Nif Slot Sniper is licensed under the MIT License.

Copyright (c) 2026 HeavyMoon

You are free to use, modify, and distribute this software, including for commercial purposes,
as long as the original copyright notice and license text are included.

The companion CLI `slottool` is a separate project with its own license and third-party notices (it uses
Mutagen, GPL-3.0); see its repository. The notices below cover this GUI only.

### Third-Party Licenses

This GUI includes third-party libraries that are distributed under their own licenses:

- Nifly (MIT License)
- Dear ImGui (MIT License)
- GLFW (zlib/libpng License)
- GLAD (MIT License)
- OpenGL Mathematics (GLM) (MIT License)
- TinyXML-2 (zlib License)
- Khronos OpenGL / OpenGL ES headers (Apache License 2.0)
- nlohmann/json (MIT License)

The full text of each license can be found in the following files:

- `LIcenses/LICENSE` — MIT License for Nif Slot Sniper
- `THIRD_PARTY_NOTICES.md` — Third-party license notices
- `LIcenses/Apache-2.0.txt` — Full text of the Apache License 2.0
- http://www.apache.org/licenses/LICENSE-2.0
