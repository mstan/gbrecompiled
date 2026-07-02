# Issues & outstanding work — inventory + perceived value

Snapshot 2026-07-02 (master @ c5d6b1a). Captures follow-up work, parked items,
the outstanding-branch inventory, and a few process gotchas, each with a value
read so future sessions can triage instead of re-discovering.

## Outstanding branches — VERIFIED against master's current code

Correction to an earlier draft: the `feature/*` and most `pr/*` branches were
first assumed to hold "stranded general bug fixes." **Verification (reading
master's CURRENT gbrt.c/ppu.c/generator.cpp, not commit subjects) shows master
already contains them all** — those branches are OLD ANCESTORS (100-160 behind)
whose fixes were carried forward during master's evolution. `git cherry` marks
them "+" but that's exact patch-id, useless across this much drift. **There is
essentially nothing to fold into master.**

Three superseded ancestors (`feature/pokemon-red-blue`,
`feature/toml-config-and-analyzer-fixes`, `pr/vblank-completion`) were pruned
2026-07-02 after verification (SHAs in the record below). Remaining branches:

| Branch | Unique | Verified status vs master | Recommendation |
|---|---|---|---|
| `pr/window-wy-trigger` | 1 | **Partly missing.** Master latches window on `WY <= LY` (ppu.c:439/456); this uses strict `LY == WY`. Subtle edge case (mid-frame WY rewrite below current line). Rare/test-ROM tier, NOT a game-affecting bug. | Low value; fold only if chasing PPU edge-case accuracy. |
| `pr/audio-underrun-fade` | 1 | Master already smooths underruns (hold-last-sample, platform_sdl.cpp:4187); this is an alternative fade-to-zero design. | Not a missing fix; a design variant. Keep as an idea or drop. |
| `merge/onto-gbrecomp` | 7 vs `gbrecomp/main` | Upstream-contribution staging. **Unpushed** — git blocks `-d` deletion. | Keep until the upstream push decision; do NOT force-delete. |
| `merge/upstream-squash` | 1 vs `origin/main` | Squash of the fork onto upstream base. **Unpushed.** | Keep pending upstream decision. |
| `wip/heuristic-eval-pre-merge` | 1 | Deliberate pre-merge backup snapshot. | Keep as a backup bookmark. |
| `debug/peanut-sav-loader` | 1 | `--sav` flag for the peanut-gb debug env. LOW. | Keep; minor tooling. |
| `main` | — | The `origin/main`-tracking line (ahead 99 of origin). | Keep — a real upstream line, not stale. |

**Net:** what remains are the two `merge/*` upstream-staging branches (unpushed,
must stay), two `pr/*` idea-variants (no unique game-affecting fix), a backup
snapshot, a debug-tooling branch, and `main`. Nothing left to fold into master.

**Deleted-branch SHA record (restorable via `git branch <name> <sha>`):**
- `accuracy/discovery` → `44d63b6` (blargg CPU-conformance scorecard; fully merged into master)
- `feat/shadow-enhancements` → `d122b2a` (shadow work; content now in master as `c5d6b1a`; `myfork/feat/shadow-enhancements` still has it)
- `feature/pokemon-red-blue` → `6208b1e` (superseded ancestor; all fixes verified in master)
- `feature/toml-config-and-analyzer-fixes` → `65fa919` (superseded ancestor; analyzer fix in master)
- `pr/vblank-completion` → `a2c30c7` (superseded; `vblank_serviced` already in master)

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

1. **Prune superseded ancestors** (their fixes are verified already in master):
   `feature/pokemon-red-blue`, `feature/toml-config-and-analyzer-fixes`,
   `pr/vblank-completion`. Record SHAs first (they're restorable). Keep them only
   if you want them as target/upstream-PR bookmarks.
2. Decide the upstream story for `merge/onto-gbrecomp` + `merge/upstream-squash`
   (they hold genuinely unpushed upstream commits — push vs park).
3. Everything else is parked-by-choice; leave it unless a concrete game needs it.
   (`pr/window-wy-trigger` holds one minor PPU edge-case refinement, not a bug.)
