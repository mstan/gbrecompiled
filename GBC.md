# GBC Support Status

Current status: real Game Boy Color support is implemented and working for representative games, but CGB accuracy work is still in progress.

## Working now

- CGB ROMs build and run through the normal recompiler flow.
- `--model auto|dmg|cgb` is supported.
- Full CGB mode and DMG-on-CGB compatibility mode are implemented.
- Core CGB hardware support is present:
  - KEY1 / double speed
  - VBK / SVBK
  - HDMA
  - CGB palettes
  - VRAM bank 1 tile attributes
  - CGB rendering and compatibility palettes
- Stable battery save IDs are implemented.
- MBC3 RTC persistence is implemented.
- Representative game status:
  - Pokemon Crystal is playable.
  - The Legend of Zelda: Link's Awakening is working well.
  - Tetris DX boots and is a useful CGB smoke target.

## Still missing

- `misc/bits/unused_hwio-C.gb` still fails.
- The curated CGB subset needs a fresh pass/fail sweep and tracking.
- Some undocumented CGB I/O readback and masking details are still incomplete.
- KEY0 / PGB edge behavior is not modeled fully.
- `FF56` infrared is still stub-level.
- Serial works well enough for current games but is not a full link-cable implementation.
- CGB timing still needs more validation:
  - double-speed edge cases
  - HDMA timing interactions
  - LCD / STAT timing edge cases
- Full CGB boot ROM emulation is still not implemented.
- There are still a few small interpreter fallback gaps in known-good CGB games.

## Recommended next work

1. Fix `unused_hwio-C`.
2. Re-run and record the curated CGB test subset.
3. Close the remaining small interpreter fallback sites in known-good CGB games.
4. Keep validating more real CGB games while tightening timing and I/O accuracy.

## Reference

When working on CGB behavior, use:

1. `tech_docs/pan_docs.md`
2. `SameBoy/`
