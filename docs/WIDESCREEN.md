# Opt-in Widescreen (Extended Horizontal View) — GB/GBC backport

Pilot title: **Megaman Xtreme 2 (GBC)**. Branch: `experiment/mmx2-widescreen`,
worktree `_wt-widescreen` (sibling of `gb-recompiled`).

This ports the widescreen convention already proven in three sibling recomps:

- **gbarecomp / MegaManZeroRecomp** — the opt-in contract (`max_view_width`
  capability gate + `extended_view_init`), margin geometry, PPU provider
  hooks, fail-closed pillarboxing.
- **nesrecomp / SuperMarioBrosRecomp** — the **OAM X16 sidecar** (the answer
  to an 8-bit OAM X on a wider-than-256 view), the `[[ram_read_hook]]`
  recompiler contract, the "**vanilla spawns + widened culling**" policy, and
  the phantom-hitbox lesson.
- snesrecomp / psxrecomp — the general "faithful sim, wider presentation"
  convention both of the above cite.

## Governing principles

Same carve-out as `docs/SHADOW_ENHANCEMENTS.md`:

1. **Opt-in, default OFF, byte-identical when off.** With no view width
   requested (or a game that has not opted in) the render path, framebuffer
   contents at native offsets, `differential.c` comparisons, frame hashes,
   and savestates are byte-identical to master. The native fast path must not
   even compute margin state.
2. **The simulation stays vanilla.** Widescreen is presentation + carefully
   classified *decision-input* shifts (cull/despawn bounds). No entity logic,
   physics, RNG, or level streaming is re-implemented. Every hook reduces
   exactly to vanilla at margin = 0.
3. **Fail-closed.** Any frame where the wide view cannot be proven sane
   (menus, title, mode transitions, unstreamed margin content) renders the
   margins as black pillarbox instead of garbage.
4. **Margins are host presentation state** — never serialized into
   savestates; caches rebuild after load (documented MMZ limitation: one
   room-reload may be needed).

## Geometry

- Native: 160×144 (10:9). BG map: 256×256 with wrap.
- `GB_MAX_EXTRA_X = 48` per side → **max render width 256** (exactly 16:9 at
  144 tall, and exactly the BG map width — beyond that wide columns would
  alias with wrap).
- Width tiers (tile-aligned, launcher/CLI vocabulary):
  `160 (native 10:9)`, `192 (4:3, +16/side)`, `224 (14:9, +32/side)`,
  `256 (16:9, +48/side)`. Odd pixel (never occurs with these tiers) would go
  right, matching gbarecomp `view_config.h`.
- Output column space: `x ∈ [-extra_left, 160+extra_right)`; framebuffer
  stride = `render_width`, native content at columns
  `[extra_left, extra_left+160)`. **When widescreen is off, stride stays
  `GB_SCREEN_WIDTH` (160) so pixel offsets are bit-identical to master.**

## Engine layers (`gb-recompiled`, game-agnostic, all inert by default)

### 1. View config + capability gate
- Request: `GBCRECOMP_VIEW_WIDTH` env → `--view-width` CLI → per-game TOML
  `[video] view_width`. Development override `GBCRECOMP_WS_WIP=1` unlocks the
  generic wide renderer for non-opted-in games (diagnostics only).
- Capability: two new `game_extras.h` hooks with inert defaults in
  `runtime/src/game_defaults/`:
  - `uint16_t game_max_view_width(void)` — default `160` (= not opted in).
  - `void game_extended_view_init(struct GBContext*, uint32_t extra_left,
    uint32_t extra_right)` — default no-op; called exactly once, only after a
    non-native width is authorized.
- Pure resolver (`view_config.h`, unit-testable): clamp request to
  `[160, min(game_max, 256)]`, split extra into left/right.

### 2. PPU wide render path (`ppu.c` / `ppu.h`)
- Framebuffers + `bg_raw_line`/`bg_priority_line` sized to
  `GB_MAX_RENDER_WIDTH(256)×144`; `ppu->render_stride`,
  `ppu->extra_left/right` (0/0/160 by default). All `scanline*GB_SCREEN_WIDTH+x`
  indexing switches to `scanline*stride + (extra_left + x)`; with margins 0
  this is the identical expression, and `differential.c`'s `sizeof` compares
  stay equal-sized on both contexts (margin tails are zero on both sides).
- `render_bg_segment` clip becomes `[-extra_left, 160+extra_right)`; the
  existing per-pixel `bg_x=(x+scx)&0xFF` sampling extends for free. Segmented
  mid-scanline (mode-3) timing semantics unchanged: dots map to *native*
  x 0..159; margin pixels for a scanline are rendered with that scanline's
  final register state at mode-3 end (margins have no hardware dot clock —
  documented approximation).
- Window layer: unchanged, drawn in native coordinates (it physically cannot
  start beyond WX=166); margins never show window content. If MMX2 uses the
  window for HUD, it stays pinned at its native position by construction.
- Sprites: pass consumes the **OAM X16 sidecar** (layer 3) when armed:
  `screen_x = g_oam_x16[slot]` (signed) instead of `oam.x - 8`; clip to the
  wide range. Sidecar off → vanilla decode, byte-identical.
- Pillarbox: `g_gbws_pillarbox`, `_left`, `_right` blank margin columns
  (fail-closed default until the game module proves the frame).

### 3. OAM X16 sidecar (runner side, `gbrt.c`)
Mirrors `nesrecomp/runner/src/runtime.c:1184-1301`:
- `g_gbws_shadow_x16[40]` — tracked when the guest writes the X byte
  (`addr&3==1`) of its **shadow OAM page** in WRAM (page registered by the
  game module; falls back to tracking the page seen at the 0xFF46 DMA write).
- Context published by the game module from inside the game's own
  "object screen X" computation (via layer-4 hooks):
  `g_gbws_obj_true_rel` (int16), `g_gbws_obj_rel8`, `g_gbws_obj_ctx_valid`.
- A game module must publish only a coherent multi-byte coordinate. MMX2 has
  both full `CB80`/`CB81` draw paths and native-only paths that update `CB80`
  alone. Its binding invalidates on the low-byte write and publishes only when
  the following high-byte write completes the pair; otherwise the sidecar
  falls back to the vanilla X byte. Publishing after either write can combine
  a new low byte with another object's stale `00`/`01`/`FF` high byte and move
  an entire metasprite by 256 pixels.
- Unwrap on write: `wide = true_rel + (int8)(byte - rel8)` with plausibility
  window (layout offsets span a few tiles) and fallback to the plain byte.
  GB nuance: OAM X is stored **+8 biased**; the sidecar stores unbiased
  screen X (`wide - 8` applied at the same point the renderer applies `-8`).
- Copied to render-side `g_oam_x16[40]` at OAM DMA (0xFF46). Direct OAM
  writes (0xFE00 pokes) fall back to plain bytes.
- Not serialized; repopulates on the next DMA.

### 4. Recompiler + interpreter hook contract
- **`[[ram_read_hook]]` in the per-game TOML** (nesrecomp contract): for each
  declared WRAM/HRAM address, the generator routes loads of that address
  through `gbrt_ram_read_hook(ctx, addr, val, pc)`; the game module switches
  on `(addr, pc)` and may return a shifted value (screen-edge shifts) or just
  observe (sidecar context publish). GB addressing wrinkle: absolute
  `LD A,(nn)` sites are statically visible; `(HL)`-indirect readers of a
  declared address can't be found statically, so the fallback is a
  runtime-side check in `gb_read8` gated on a small sorted table (only when
  widescreen is armed — zero cost otherwise). Start with the runtime-side
  hook (correct for both generated code and the interpreter fallback, which
  MMX2 still uses for a few functions); add static emission later only if
  profiling demands it.
- **`[[imm_override]]`** (gbarecomp `thumb_alu_immediate_override` analog):
  route a specific `CP n`/`ADD n`/`SUB n` at bank:PC through
  `gbrt_imm_override8(ctx, bank, pc, orig)`. Needs both generator emission
  (IR `lower_alu_imm`) and an interpreter-side consult at the same PC. Only
  built if the MMX2 cull/cull bounds turn out to be immediates rather than
  RAM edge reads (TBD by RE).

### 5. Present path (`platform_sdl.cpp`)
- Streaming GL texture, upload loop, `s_upload_buf`, PPM dumps,
  `border_content_width()`, scale presets, and viewport math parameterized on
  `render_width` (recreate texture on width change). SGB border + widescreen
  are mutually exclusive (border wins; widescreen pillarboxes to native).
- Color LUT / color-correction interact per-pixel and are width-agnostic.

### 6. Verification
- `differential.c` / co-sim always run native (they never arm widescreen);
  off = byte-identical is asserted by existing gates.
- A `ws_check`-style probe (nesrecomp `tools/ws_check.py` pattern) drives the
  TCP debug server and asserts the three historical failure modes: wrap
  ghosts (sprite drawn at wrapped X), spawn pop-in *inside* the view, despawn
  pop-out. Plus live windowed play for visual/audio verification.

## MMX2 game module (`Megaman Xtreme 2/extras.c`)

Policy is **vanilla spawns + vanilla culling**, sidecar for sprites,
fail-closed gameplay-mode gate, HUD pinned. MMX2's stock horizontal object
window already encloses the complete 256-pixel presentation window.

Game-specific bindings (from the generated-C RE pass; verify live before
shipping):

| Purpose | Binding |
|---|---|
| Camera X world (16-bit) | `0xCA08` lo / `0xCA09` hi; SCX shadow `0xCAE0`; chain `0xCA08→0xCAE0→0xFF43` (`bank_1d 0x44c4`) |
| Camera Y world (16-bit) | `0xCA05/06`; SCY shadow `0xCADF` |
| Shadow OAM | page `0xC2` (DMA write `01:4a34`); OAM cursor `0xCAD5`, overflow flag `0xCAD6` |
| Sidecar context | **`0xCB80/0xCB81` = current draw object's 16-bit screen X** (lo/hi), `0xCB82` = screen Y — published by the game itself before the metasprite writer `func_1320 @ 00:1323` adds per-tile offsets to the low byte. Context publish = read hook on `0xCB80` (or entry of the OAM-build dispatcher `00:12db`). |
| On-screen cull (shared by draw-skip, despawn, AND buster shots) | `func_34d6 @ 00:34d6`: on-screen iff `(relX+0x40) < 0x138` → relX ∈ [−64, +248) (and `(relY+0x40) < 0x12C`). The 256-pixel view is [−48,+208), leaving 16 pixels of native guard on the left and 40 on the right. The reviewed `[[imm_override]]` sites are deliberately not enabled: widening to [−112,+296) only increases active-object/OAM pressure. |
| HUD | Window layer: `WY←0xCAE1 @ 00:0420`, `WX←0xCAE2 @ 00:0425`. Pinned native by construction (engine renders window only in native columns). |
| Spawns | Column-streaming driven (`func_22af @ 00:22af` camera-crossing detector → `func_1779` loaders). Left vanilla; margin enemies appear once their column streams. |
| Object fields | X `+8/+9`, Y `+5/+6`, active `+0`, flags `+0x0F`, sprite slot `+0x44` (entity page in HRAM `0xFFA2`) |

Unconfirmed (flagged by RE, verify in-emulator): no standalone spawn-list
scan against `camX±width` was found (believed streaming-driven); no
buster-specific cull tighter than `func_34d6`.

Known constraint to respect: MMX2's right-margin content depends on how far
ahead of the camera its column streamer writes the BG map (256-wide map −
160 view = 96px of off-screen columns, minus streamer lead). Effective margin
caps will be measured, SMB-style (`right ≤ streamer lead`, `left ≤ map wrap`),
and clamped in the module, with the MMZ synthetic-streamer trick held in
reserve only if the natural caps prove too tight.

### Streaming-ring lead widening

Live tracing resolved the apparent corner wrap: MMX2 treats the 32-column BG
map as a streaming ring and refreshes one 16-pixel strip at camera crossings.
During the observed horizontal scroll, accepted writes came from the paired
tilemap writers at `00:0b55/0b57` and `00:0b65/0b67`; only ring columns
`0..7` and `24..31` were refreshed. A 256-pixel render exposes all 32 columns,
including cells beyond the native streamer lead.

The analogous Mega Man X6 work first tried clearing/masking revealed columns.
That hid valid authored layers and produced moving black trim. Its durable fix
was to refill the tile ring so the population window matches the render
window. MMX2 now follows that same policy through the armed-only per-frame
extended-view update hook:

- `func_1923 -> func_0b07` reads the 16-bit camera X at `0xCA08/09` and
  converts it to the metatile coordinates consumed by the four strip writers;
- horizontal direction 1 natively streams at camera + 11 metatiles, while
  direction 2 streams at camera - 1 metatile;
- `func_0ab1` resolves a world metatile through the bank-5 `DA00` page table;
  the selected page points into the active level-data ROM bank;
- `func_0b4d` expands that metatile through ROM tables `40xx..43xx` (tiles)
  and `44xx..47xx` (CGB attributes) into a 2x2 block at `9800`;
- the MMX2 module clones those two routines byte-for-byte as a synthetic
  margin tile source, selecting the correct 8x8 quadrant from full 16-bit
  world coordinates;
- before the cache is trusted, it searches for a ROM bank whose two native
  streamer-edge columns match the live tilemap with zero mismatches. Menus,
  transitions, an unsupported layer, or a bad reverse-engineering assumption
  therefore remain pillarboxed;
- the cache keeps the two extreme partial metatiles independent when an
  unaligned 256-pixel view spans 17 metatiles but the hardware ring has only
  16 slots;
- negative world X is finite-map void and returns black instead of wrapping
  to the far end of the level.

The generic `game_extended_view_update()` hook is a no-op by default and is
called only from the already-armed margin path. The tile-source hook is queried
only for margin pixels and never mutates guest VRAM; the native framebuffer and
default game behavior are unchanged.

### Frame-stable margin palettes

Synthetic columns do not have hardware dot timing. Sampling CGB palette RAM at
each scanline's mode-3 end exposed MMX2's within-frame palette changes as
flickering horizontal bands in otherwise solid margin tiles. Armed views now
snapshot BG/OBJ palette RAM plus the DMG palette registers on line 0 and use
that stable copy only for synthetic margin pixels. Native pixels retain their
cycle-live palette behavior, and the snapshot is untouched at native width.
The MMX2 module likewise snapshots its 16-bit camera X/Y on line 0; consulting
live guest camera fields in every later scanline had torn scrolling margins
into intermittent vertical slivers.

## Phases

1. **Engine framework**: shipped wide render, sidecar, capability gate,
   per-frame update hook, and width-aware presentation.
2. **Reviewed generated hook**: the `[[imm_override]]` contract remains
   available, but MMX2 does not enable it because its native cull already
   covers 256 pixels; no generated C edits.
3. **MMX2 module**: local opt-in module now includes a validated synthetic
   margin cache plus sprite sidecar and native-pressure culling.
4. **Remaining validation**: broader live play (enemies, shots, bosses,
   savestates) and optional full-width dialogue/window work.
