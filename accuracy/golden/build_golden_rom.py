#!/usr/bin/env python3
"""Build a minimal MBC0 GB ROM that exercises the structurally-fragile operand
patterns (Axis 1 golden test): the (HL) RMW / ALU family (magic index 6) and
PUSH/POP AF flag masking (magic index 4). No assembler needed — raw opcodes.

The ROM both (a) is recompiled so the generated C can be asserted structurally
(memory read at HL for every (HL) op; F low-nibble masking for POP AF), and
(b) runs to an infinite loop leaving an ISA-correct signature in B/C/D/E so a
behavioral check can read it via GBRT_REGS_LOG.

Expected final signature: B=0x0F C=0x3F D=0xF0 E=0xF0 (H=0xC0 L=0x00).

Usage: build_golden_rom.py <out.gb>
"""
import sys

def build():
    rom = bytearray(b"\x00" * 0x8000)  # 32 KiB, ROM-only

    # ---- entry: 0x100 NOP; JP 0x150 ----
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])

    # ---- Nintendo logo (0x104-0x133): real bytes (harmless; recomp skips boot) ----
    logo = bytes([
        0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
        0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
        0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E,
    ])
    rom[0x104:0x104 + len(logo)] = logo

    # ---- header: title, cart type 0x00 (ROM only), 32KB, no RAM ----
    title = b"GOLDEN-HL-AF"
    rom[0x134:0x134 + len(title)] = title
    rom[0x147] = 0x00  # MBC0
    rom[0x148] = 0x00  # 32 KiB
    rom[0x149] = 0x00  # no RAM

    # ---- test code at 0x150 ----
    # HL stays = 0xC000 (data pointer) the whole time; results are stashed to
    # 0xC001..0xC006 and reloaded into B/C/D/E at the end so nothing collides.
    code = bytes([
        0x21, 0x00, 0xC0,        # LD HL, 0xC000
        0x36, 0x0F,              # LD (HL), 0x0F          ; mem[C000]=0x0F
        # AND (HL): 0xFF & 0x0F = 0x0F
        0x3E, 0xFF,              # LD A, 0xFF
        0xA6,                    # AND (HL)               ; A=0x0F
        0xEA, 0x01, 0xC0,        # LD (0xC001), A
        # OR (HL): 0x30 | 0x0F = 0x3F
        0x3E, 0x30,              # LD A, 0x30
        0xB6,                    # OR (HL)                ; A=0x3F
        0xEA, 0x02, 0xC0,        # LD (0xC002), A
        # XOR (HL): 0xFF ^ 0x0F = 0xF0
        0x3E, 0xFF,              # LD A, 0xFF
        0xAE,                    # XOR (HL)               ; A=0xF0
        0xEA, 0x03, 0xC0,        # LD (0xC003), A
        # ADD A,(HL): 0x01 + 0x0F = 0x10
        0x3E, 0x01,              # LD A, 0x01
        0x86,                    # ADD A,(HL)             ; A=0x10
        0xEA, 0x04, 0xC0,        # LD (0xC004), A
        # INC (HL) then DEC (HL): 0x0F -> 0x10 -> 0x0F (RMW net zero)
        0x34,                    # INC (HL)
        0x35,                    # DEC (HL)
        0x7E,                    # LD A,(HL)              ; A=0x0F
        0xEA, 0x05, 0xC0,        # LD (0xC005), A
        # POP AF flag masking: dirty F low nibble via stack, must read back 0xF0
        0x01, 0xFF, 0x12,        # LD BC, 0x12FF
        0xC5,                    # PUSH BC                ; stack: F'=0xFF, A'=0x12
        0xF1,                    # POP AF                 ; A=0x12, F=0xFF&0xF0=0xF0
        0xF5,                    # PUSH AF                ; pushes af & 0xFFF0
        0xD1,                    # POP DE                 ; D=0x12, E=0xF0
        0x7B,                    # LD A, E                ; A=0xF0 (masked F)
        0xEA, 0x06, 0xC0,        # LD (0xC006), A
        # reload signature: B=AND, C=OR, D=XOR, E=maskedF
        0xFA, 0x01, 0xC0, 0x47,  # LD A,(0xC001); LD B,A  ; B=0x0F
        0xFA, 0x02, 0xC0, 0x4F,  # LD A,(0xC002); LD C,A  ; C=0x3F
        0xFA, 0x03, 0xC0, 0x57,  # LD A,(0xC003); LD D,A  ; D=0xF0
        0xFA, 0x06, 0xC0, 0x5F,  # LD A,(0xC006); LD E,A  ; E=0xF0
        0x18, 0xFE,              # JR -2 (spin)
    ])
    rom[0x150:0x150 + len(code)] = code

    # ---- header checksum (0x14D) over 0x134..0x14C ----
    cks = 0
    for i in range(0x134, 0x14D):
        cks = (cks - rom[i] - 1) & 0xFF
    rom[0x14D] = cks

    return rom

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: build_golden_rom.py <out.gb>", file=sys.stderr)
        sys.exit(2)
    data = build()
    with open(sys.argv[1], "wb") as f:
        f.write(data)
    print(f"wrote {sys.argv[1]} ({len(data)} bytes)")
