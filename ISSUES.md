# Issues & outstanding work — inventory + perceived value

Snapshot 2026-07-02 (master @ c5d6b1a). Captures follow-up work, parked items,
the outstanding-branch inventory, and a few process gotchas, each with a value
read so future sessions can triage instead of re-discovering.

## Outstanding branches (all hold unique/unpushed work — none stale)

The repo audit found the tree was already fairly clean. Only one truly-redundant
label existed (`accuracy/discovery`, fully merged) — deleted. Everything below
carries unique work; several are candidates to fold into master.

| Branch | Unique | What / value | Recommendation |
|---|---|---|---|
| `feature/pokemon-red-blue` | 19 | Red/Blue support **+ real bug fixes**: frame timing when LCD disabled (NPC dialogue audio glitch), serial transfer timing (+4096 T-cycle delay), suppress serial IRQ w/ no link cable, ROM picker + CRC, configurable keybinds, multi-CRC. **HIGH** | Cherry-pick the *general* fixes (LCD-off timing, serial) into master; they aren't Red/Blue-specific. |
| `feature/toml-config-and-analyzer-fixes` | 1 | TOML per-game config + **analyzer fall-through bug fix** + serial transfer fix. **MED-HIGH** | The analyzer fix is a real correctness fix — candidate for master. |
| `pr/audio-underrun-fade` | 1 | SDL: fade underrun tail from last sample instead of silence. **MED** | Prepared for `origin/main`. Decide: push upstream vs merge to master. |
| `pr/vblank-completion` | 1 | Let `gb_run_frame` complete the VBlank handler before returning. **MED** | Prepared for `origin/main`. Same decision. |
| `pr/window-wy-trigger` | 1 | PPU: latch window trigger on exact `LY == WY`, not `>=`. **MED** | Prepared for `origin/main`. Same decision. |
| `merge/onto-gbrecomp` | 7 vs `gbrecomp/main` | Upstream-contribution staging (ANGLE fix, SHA-256 verify, agnostic hooks, Tier-0 manifest). **Unpushed** — git blocked `-d` deletion. | Keep until the upstream push decision; do NOT force-delete. |
| `merge/upstream-squash` | 1 vs `origin/main` | Squash of the 30-commit fork onto upstream base. **Unpushed.** | Keep pending upstream decision. |
| `wip/heuristic-eval-pre-merge` | 1 | Deliberate pre-merge backup snapshot (heuristic-eval + analyzer). | Keep as a backup bookmark. |
| `debug/peanut-sav-loader` | 1 | `--sav` flag to load a battery save into the peanut-gb debug env. **LOW** | Keep; minor tooling. |
| `main` | — | The `origin/main`-tracking line (ahead 99 of origin). | Keep — it's a real upstream line, not stale. |

**Deleted-branch SHA record (restorable via `git branch <name> <sha>`):**
- `accuracy/discovery` → `44d63b6` (blargg CPU-conformance scorecard; fully merged into master)
- `feat/shadow-enhancements` → `d122b2a` (shadow work; content now in master as `c5d6b1a`; `myfork/feat/shadow-enhancements` still has it)

## Parked (diminishing returns — do NOT resume without a real-game need)

- **Cycle-exact per-M-cycle timing.** Honest coordinate: mem_timing DMG oracle
  diverges at instr **2,492,056**, a clean −4 T-cycle (1 M-cycle) memory-access
  timing gap (all divider/PPU/interrupt state matches). Remaining phases fix
  **test-ROM scores only** (blargg mem_timing 01/02, 4 mooneye timer ROMs,
  mooneye ei_/di_/reti_, PPU sub-scanline) with ~zero player-visible impact.
  The `div=8` power-on fix + honest oracle are the keepers. Full spec:
  `CYCLE_EXACT_INITIATIVE.md`. Implementing the read/write split needs Ghidra on
  `mem_timing.gb` (C003–C00F WRAM helper + C2C0–C2C9 harness), not currently
  stood up.
- **MMX2 CGB PPU LY-phase drift** (oracle instr 81,598: recomp reaches LY=0x90
  ~4 system-cycles late). Same PPU sub-scanline family; test-ROM/oracle only.
- **Audio spectral residual** (~0.95 cosine vs SameBoy). No *confirmed audible*
  defect; the one filed "note-drop" was an oracle capture artifact. Not pursued.

## Process gotchas

- **Stale-artifact trap.** Each generated per-game project links `../runtime` as
  a subdir build. After ANY `runtime/src` edit, rebuild the SPECIFIC artifact
  (`ninja -C <artifact>/build`), not just `build/`, or you measure/run a stale
  runtime. (This silently invalidated a prior mem_timing measurement.)
- **`rom.cfg` needs a Windows path.** The launcher reads `rom.cfg` next to the
  exe; it must hold `F:/...` (not `/f/...`) or you get the ROM picker.
- **No push to any remote** without explicit instruction. `myfork` is the user's;
  `origin`/`gbrecomp` are upstreams. master is ahead of `myfork/master`, all local.

## Suggested next moves (if/when picking work back up)

1. Fold the **general bug fixes** out of `feature/pokemon-red-blue` and
   `feature/toml-config-and-analyzer-fixes` into master (LCD-off frame timing,
   serial timing, analyzer fall-through) — real correctness, not game-specific.
2. Decide the upstream story for the `pr/*` + `merge/*` branches (push vs park).
3. Everything else here is parked-by-choice; leave it unless a concrete game needs it.
