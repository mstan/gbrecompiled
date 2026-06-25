# Ghidra Setup for GB Recompiled (Tetris)

Step-by-step instructions to get Ghidra analyzing our Tetris ROM so Claude can
query disassembly/decompilation via MCP.

---

## One-Time Setup

### 1. Install GhidraBoy (SM83 processor support)

Game Boy uses the SM83 (LR35902) CPU. Ghidra doesn't know it natively.

1. Download the latest release from: https://github.com/Gekkio/GhidraBoy/releases
   - Grab the `.zip` that matches your Ghidra version (e.g. `ghidra_11.3_PUBLIC_...`)
2. In Ghidra: **File > Install Extensions > "+" button** > select the downloaded `.zip`
3. Restart Ghidra

### 2. Install the Ghidra MCP Server plugin

This lets Claude query Ghidra over HTTP (SSE on port 4000).

1. Download from: https://github.com/LaurieWired/GhidraMCP/releases
   - Or whichever MCP bridge you used for the NES projects
2. Install same way: **File > Install Extensions > "+"** > select the `.zip`
3. Restart Ghidra

---

## Per-Session Setup

### 3. Create a new Ghidra project

1. **File > New Project** > Non-Shared Project
2. Name: `tetris_gb` (or whatever you like)
3. Directory: `F:\Projects\gbcrecomp\gb-recompiled\`

### 4. Import Bank 0 (0x0000-0x3FFF)

Bank files are already extracted at `banks/bank0.bin` and `banks/bank1.bin`.

1. **File > Import File** > select `banks/bank0.bin`
2. In the Import dialog:
   - **Format:** Raw Binary
   - **Language:** `SM83` (from GhidraBoy) > Little Endian > 16-bit > default
   - Click **Options...** and set **Base Address: `0x0000`**
3. Click OK / Import
4. When prompted to analyze, click **Yes** (use all default analyzers)
5. Wait for auto-analysis to finish (should be fast, it's only 16KB)

### 5. Import Bank 1 (0x4000-0x7FFF)

1. **File > Import File** > select `banks/bank1.bin`
2. Same settings as above EXCEPT:
   - **Base Address: `0x4000`**
3. Click OK / Import, analyze with defaults

> **Why separate banks?** Tetris is ROM-only (no MBC), so both are always mapped.
> Loading them separately lets us keep Ghidra's address space matching the Game Boy's
> actual memory map. Bank 0 code lives at 0x0000-0x3FFF, bank 1 at 0x4000-0x7FFF.
>
> **Alternative:** You can also just import the whole `roms/tetris.gb` (32KB) as a
> single raw binary at base address 0x0000. Since Tetris has no bank switching, this
> works fine and is simpler. The two-file approach is mainly useful for MBC games
> where each switchable bank would be a separate Ghidra program loaded at 0x4000.

### 6. Verify the import worked

In bank0, go to address **0x0100** (the ROM entry point). You should see:
```
0100: NOP
0101: JP 0x0150
```

Key interrupt vectors to check:
```
0x0040: VBlank handler (should have real code, not 0x00/NOP)
0x0048: STAT handler
0x0050: Timer handler
```

### 7. Start the MCP server

1. In Ghidra's CodeBrowser, go to **Window > Script Manager**
2. Find the MCP server script (should be listed after installing the MCP plugin)
3. Run it — it will start an HTTP/SSE server
4. **Set the port to 4000** (check the script's config or startup dialog)
5. You should see a console message like: `MCP server listening on port 4000`

### 8. Connect Claude Code

In your Claude Code session:
```
/mcp
```
This will reconnect to the MCP servers defined in `.mcp.json` (already configured for port 4000).

Claude will verify with `mcp__ghidra__get_program_info` to confirm the connection.

---

## What We're Looking For

The recompiler missed these addresses — they fall back to the interpreter at runtime:

| Address | Bank | Hits | Notes |
|---------|------|------|-------|
| 0x0257  | 0    | Few  | Gap between 0x0254 and 0x028B |
| 0x025A  | 0    | Few  | Same gap |
| 0x027A  | 0    | Few  | Same gap |
| 0x0325  | 0    | Few  | Init-related |
| 0x033B  | 0    | Few  | Init-related |
| 0x034C  | 0    | Few  | Main loop area |
| 0x035A  | 0    | Few  | Main loop area |
| 0x035F  | 0    | Few  | Main loop area |
| 0x036C  | 0    | **Hot** | Main loop idle — fires every frame |
| 0x0377  | 0    | Few  | Main loop area |
| 0x7FF0  | 1    | Few  | End of bank 1 |
| 0x7FF3  | 1    | Few  | End of bank 1 |
| 0xFFB6  | HRAM | Few  | OAM DMA return path |

Once Ghidra is running, Claude can look at each address to understand:
1. What the code does
2. Why the analyzer missed it (jump table? computed call? mid-function entry?)
3. Whether to add it as an `--add-entry-point` or fix the analyzer

---

## Quick Reference

| Item | Value |
|------|-------|
| ROM | `roms/tetris.gb` (32KB, no MBC) |
| Banks | `banks/bank0.bin` (at 0x0000), `banks/bank1.bin` (at 0x4000) |
| Processor | SM83 (GhidraBoy) |
| MCP port | 4000 |
| MCP config | `.mcp.json` |
| Entry point | 0x0100 |
| VBlank | 0x0040 |
| STAT | 0x0048 |
| Timer | 0x0050 |
