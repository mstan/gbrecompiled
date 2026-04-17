#!/usr/bin/env python3
"""Parse OAM hex dump and display sprite info."""
import sys

hex_data = sys.argv[1] if len(sys.argv) > 1 else input("hex: ")
data = bytes.fromhex(hex_data)

for i in range(0, min(len(data), 160), 4):
    y = data[i]
    x = data[i+1]
    tile = data[i+2]
    flags = data[i+3]
    sprite_num = i // 4

    # Screen position (OAM Y is offset by 16, X by 8)
    screen_y = y - 16
    screen_x = x - 8

    if y == 0 and x == 0 and tile == 0 and flags == 0:
        continue  # empty sprite

    palette = "OBP1" if (flags & 0x10) else "OBP0"
    xflip = "X" if (flags & 0x20) else " "
    yflip = "Y" if (flags & 0x40) else " "
    behind = "BG" if (flags & 0x80) else "  "

    visible = 0 <= screen_y < 144 and 0 <= screen_x < 168
    vis = "VIS" if visible else "   "

    print(f"  #{sprite_num:2d}: Y={screen_y:4d} X={screen_x:4d} Tile=0x{tile:02X} {palette} {xflip}{yflip} {behind} {vis}")
