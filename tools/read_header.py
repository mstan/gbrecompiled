import glob, os

roms = glob.glob("F:/Projects/gbcrecomp/gb-recompiled/roms/*.gb")
for r in roms:
    data = open(r, "rb").read(0x150)
    print("--- " + os.path.basename(r) + " ---")
    print("  Entry point (0x100):", data[0x100:0x104].hex())
    print("  Title (0x134):", data[0x134:0x144])
    print("  MBC type (0x147):", hex(data[0x147]))
    print("  Version (0x14C):", hex(data[0x14C]))
    print("  First 4 bytes (0x000):", data[0:4].hex())
