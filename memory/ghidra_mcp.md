# Ghidra MCP Server — GB Recompiled

Reference disassembler for debugging generated code and understanding ROM behavior.

## Setup (one-time)

1. **Install GhidraBoy** — adds SM83 (LR35902) processor support to Ghidra:
   https://github.com/Gekkio/GhidraBoy
   Drop the extension JAR into Ghidra's `Extensions/` folder and install via File > Install Extensions.

2. **MCP config** — add to `~/.claude.json` mcpServers:
   ```json
   "ghidra": { "type": "sse", "url": "http://localhost:8080/sse" }
   ```

3. **Load ROM** (each session):
   - File > New Project → import `roms/tetris.gb`
   - Processor: `SM83` (from GhidraBoy) or `Z80` if GhidraBoy not installed
   - Language: Little Endian, 16-bit address space
   - Base address: `0x0000`
   - DO NOT strip header — ROM header is at 0x0100–0x014F (entry point at 0x0100)

4. **Start MCP server** inside Ghidra (Script Manager → run MCP bridge script)

5. **Reconnect**: `/mcp` in Claude Code, then verify with `mcp__ghidra__get_program_info`

## Address Mapping

```
ROM bank 0:  0x0000–0x3FFF  (always mapped)
ROM bank N:  0x4000–0x7FFF  (switchable via MBC)
VRAM:        0x8000–0x9FFF
External RAM:0xA000–0xBFFF
WRAM:        0xC000–0xDFFF
OAM:         0xFE00–0xFE9F
I/O:         0xFF00–0xFF7F
HRAM:        0xFF80–0xFFFE
IE register: 0xFFFF
```

For banked addresses, Ghidra sees the flat ROM. Physical offset for bank N, addr A:
```
offset = N × 0x4000 + (A − 0x4000)   [for A in 0x4000–0x7FFF]
offset = A                              [for A in 0x0000–0x3FFF, always bank 0]
```

The recompiler names functions as `func_NN_AAAA` where NN=bank (hex), AAAA=address (hex).
Example: `func_01_5234` → bank 1, address 0x5234 → Ghidra offset 0x5234.

## Key Entry Points (Tetris)

| Address | Purpose |
|---------|---------|
| 0x0100 | ROM entry point (NOP + JP 0x0150) |
| 0x0040 | VBlank interrupt handler |
| 0x0048 | STAT interrupt handler |
| 0x0050 | Timer interrupt handler |
| 0x0150 | Game init (post-boot) |

## Tools Available

| Tool | What it does |
|------|-------------|
| `mcp__ghidra__get_program_info` | Verify Ghidra is running (call this first) |
| `mcp__ghidra__get_code addr` | Disassembly + decompiled C at address |
| `mcp__ghidra__list_functions` | All known functions in ROM |
| `mcp__ghidra__get_xrefs addr` | What calls/jumps to this address |
| `mcp__ghidra__search_memory pattern` | Find byte patterns in ROM |

## Debugging Workflow

### Finding a missing entry point
```
1. Runtime logs: [GB] Interpreter called at bank:addr
2. mcp__ghidra__get_code 0xADDR  ← understand what it does
3. Determine: is this reachable from a known path, or a jump table entry?
4. Add --add-entry-point NN:AAAA to recompiler invocation
5. Regenerate + rebuild
```

### Investigating a PPU/timing bug
```
1. Identify the symptom (wrong frame, corruption after specific game event)
2. In Ghidra, find the VBlank handler (0x0040) and the game's main loop
3. mcp__ghidra__get_code on the relevant routine
4. Understand what LCDC/STAT/LY values the game expects
5. Fix ppu.c accordingly, log in ppu.log
```

### Cross-referencing interpreter calls
```
1. grep logs/ for "[GB] Interpreter"
2. Collect all unique bank:addr pairs
3. mcp__ghidra__get_xrefs on each — find who calls them
4. Determine if the caller was recompiled (fix coverage) or is also an interpreter call (recurse)
```

## Port
- Ghidra MCP: 4000 (SSE)
