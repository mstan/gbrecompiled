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

Policy mirrors SMB's classification: **vanilla spawns + widened culling**,
sidecar for sprites, fail-closed gameplay-mode gate, HUD pinned.

Game-specific bindings (from the generated-C RE pass; verify live before
shipping):

| Purpose | Binding |
|---|---|
| Camera X world (16-bit) | `0xCA08` lo / `0xCA09` hi; SCX shadow `0xCAE0`; chain `0xCA08→0xCAE0→0xFF43` (`bank_1d 0x44c4`) |
| Camera Y world (16-bit) | `0xCA05/06`; SCY shadow `0xCADF` |
| Shadow OAM | page `0xC2` (DMA write `01:4a34`); OAM cursor `0xCAD5`, overflow flag `0xCAD6` |
| Sidecar context | **`0xCB80/0xCB81` = current draw object's 16-bit screen X** (lo/hi), `0xCB82` = screen Y — published by the game itself before the metasprite writer `func_1320 @ 00:1323` adds per-tile offsets to the low byte. Context publish = read hook on `0xCB80` (or entry of the OAM-build dispatcher `00:12db`). |
| On-screen cull (shared by draw-skip, despawn, AND buster shots) | `func_34d6 @ 00:34d6`: on-screen iff `(relX+0x40) < 0x138` → relX ∈ [−64, +248) (and `(relY+0x40) < 0x12C`). Widen via `[[imm_override]]`: `ADD A,$40 @ 00:34e9` → `0x40+extra_left`; `SUB $38 @ 00:34f2` (low byte of 0x138) → `0x38+extra_left+extra_right` (no carry for extras ≤ 96, high-byte `SBC $01` untouched). |
| HUD | Window layer: `WY←0xCAE1 @ 00:0420`, `WX←0xCAE2 @ 00:0425`. Pinned native by construction (engine renders window only in native columns). |
| Spawns | Column-streaming driven (`func_22af @ 00:22af` camera-crossing detector → `func_1779` loaders). **Left vanilla** (SMB policy: native spawns + wide culling); margin enemies appear once their column streams — accepted pop-in at the streaming edge. |
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

## Phases

1. **Engine framework** (layers 1-3, 5): wide render + sidecar + config,
   `GBCRECOMP_WS_WIP=1` proves the generic path on MMX2 (BG margins visible,
   sprites vanilla-pinned, pillarbox working). Regen nothing; runtime-only.
2. **Hook contract** (layer 4): runtime-side `ram_read_hook` table +
   interpreter parity; TOML plumbing.
3. **MMX2 module**: bindings from the RE report; measure streamer lead; ship
   tiers that survive `ws_check` + live play.
4. **Verification + docs**: probe, README (opt-in instructions, caveats),
   ENHANCEMENTS.md entry.
