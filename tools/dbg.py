#!/usr/bin/env python3
"""
dbg.py -- TCP debug client for GB recomp projects

Usage:
    python dbg.py [--port N] [--host H] [command] [args...]
    python dbg.py                           # interactive REPL (port 4370)
    python dbg.py --port 4371 regs          # query Peanut-GB emulator

Commands:
    ping                        Heartbeat check
    frame                       Current frame number
    regs / get_registers        Dump CPU registers (A,F,B,C,D,E,H,L,SP,PC)
    read <addr> [len]           Read RAM bytes (hex)
    write <addr> <hex>          Write RAM bytes
    oam [index]                 Read OAM data (all 40 sprites or specific index)
    vram <addr> [len]           Read VRAM bytes
    ppu                         PPU register state (LCDC, STAT, SCY, SCX, etc.)
    io <addr> [len]             Read I/O registers (0xFF00-0xFF7F)
    mapper                      Current ROM/RAM bank state
    watch <addr>                Set byte watchpoint
    unwatch <addr>              Remove watchpoint
    pause                       Pause game
    continue / c                Resume game
    step [n]                    Step N frames (default 1)
    run_to <frame>              Run to specific frame
    history                     Ring buffer stats
    get_frame <n>               Get frame record
    range <start> <end>         Frame range query
    ts <start> <end>            Frame timeseries (compact)
    input <buttons_hex>         Override joypad input
    clear_input                 Remove input override
    quit                        Quit game
"""

import json
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4370


def connect(host=DEFAULT_HOST, port=DEFAULT_PORT):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    s.settimeout(5.0)
    return s


def send_cmd(sock, cmd_dict):
    line = json.dumps(cmd_dict) + "\n"
    sock.sendall(line.encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf.decode().strip())


def pretty_regs(resp):
    if not resp.get("ok"):
        return json.dumps(resp, indent=2)
    lines = []
    lines.append(f"  A: {resp.get('A','?')}  F: {resp.get('F','?')}   "
                 f"[Z:{resp.get('Z',0)} N:{resp.get('N',0)} "
                 f"H:{resp.get('H',0)} C:{resp.get('C',0)}]")
    lines.append(f"  B: {resp.get('B','?')}  C: {resp.get('C_reg','?')}")
    lines.append(f"  D: {resp.get('D','?')}  E: {resp.get('E','?')}")
    lines.append(f"  H: {resp.get('H_reg','?')}  L: {resp.get('L','?')}")
    lines.append(f"  SP: {resp.get('SP','?')}  PC: {resp.get('PC','?')}")
    lines.append(f"  IME: {resp.get('IME','?')}  ROM bank: {resp.get('rom_bank','?')}  "
                 f"RAM bank: {resp.get('ram_bank','?')}")
    lines.append(f"  Frame: {resp.get('frame','?')}")
    return "\n".join(lines)


def pretty_json(resp):
    return json.dumps(resp, indent=2)


def run_command(sock, args):
    if not args:
        return
    cmd = args[0].lower()

    if cmd == "ping":
        return pretty_json(send_cmd(sock, {"cmd": "ping"}))
    elif cmd == "frame":
        return pretty_json(send_cmd(sock, {"cmd": "frame"}))
    elif cmd in ("regs", "get_registers"):
        return pretty_regs(send_cmd(sock, {"cmd": "get_registers"}))
    elif cmd == "read":
        addr = args[1] if len(args) > 1 else "0xC000"
        length = int(args[2]) if len(args) > 2 else 16
        return pretty_json(send_cmd(sock, {"cmd": "read_ram", "addr": addr, "len": length}))
    elif cmd == "write":
        if len(args) < 3:
            return "Usage: write <addr> <hex>"
        return pretty_json(send_cmd(sock, {"cmd": "write_ram", "addr": args[1], "hex": args[2]}))
    elif cmd == "oam":
        idx = int(args[1]) if len(args) > 1 else -1
        if idx >= 0:
            return pretty_json(send_cmd(sock, {"cmd": "read_oam", "index": idx}))
        return pretty_json(send_cmd(sock, {"cmd": "read_oam"}))
    elif cmd == "vram":
        addr = args[1] if len(args) > 1 else "0x8000"
        length = int(args[2]) if len(args) > 2 else 16
        return pretty_json(send_cmd(sock, {"cmd": "read_vram", "addr": addr, "len": length}))
    elif cmd == "ppu":
        return pretty_json(send_cmd(sock, {"cmd": "ppu_state"}))
    elif cmd == "io":
        addr = args[1] if len(args) > 1 else "0xFF00"
        length = int(args[2]) if len(args) > 2 else 1
        return pretty_json(send_cmd(sock, {"cmd": "read_io", "addr": addr, "len": length}))
    elif cmd == "mapper":
        return pretty_json(send_cmd(sock, {"cmd": "mapper_state"}))
    elif cmd == "watch":
        if len(args) < 2:
            return "Usage: watch <addr>"
        return pretty_json(send_cmd(sock, {"cmd": "watch", "addr": args[1]}))
    elif cmd == "unwatch":
        if len(args) < 2:
            return "Usage: unwatch <addr>"
        return pretty_json(send_cmd(sock, {"cmd": "unwatch", "addr": args[1]}))
    elif cmd == "pause":
        return pretty_json(send_cmd(sock, {"cmd": "pause"}))
    elif cmd in ("continue", "c"):
        return pretty_json(send_cmd(sock, {"cmd": "continue"}))
    elif cmd == "step":
        n = int(args[1]) if len(args) > 1 else 1
        return pretty_json(send_cmd(sock, {"cmd": "step", "count": n}))
    elif cmd == "run_to":
        if len(args) < 2:
            return "Usage: run_to <frame>"
        return pretty_json(send_cmd(sock, {"cmd": "run_to_frame", "frame": int(args[1])}))
    elif cmd == "history":
        return pretty_json(send_cmd(sock, {"cmd": "history"}))
    elif cmd == "get_frame":
        if len(args) < 2:
            return "Usage: get_frame <n>"
        return pretty_json(send_cmd(sock, {"cmd": "get_frame", "frame": int(args[1])}))
    elif cmd == "range":
        if len(args) < 3:
            return "Usage: range <start> <end>"
        return pretty_json(send_cmd(sock, {"cmd": "frame_range",
                                            "start": int(args[1]), "end": int(args[2])}))
    elif cmd == "ts":
        if len(args) < 3:
            return "Usage: ts <start> <end>"
        return pretty_json(send_cmd(sock, {"cmd": "frame_timeseries",
                                            "start": int(args[1]), "end": int(args[2])}))
    elif cmd == "input":
        if len(args) < 2:
            return "Usage: input <buttons_hex>"
        return pretty_json(send_cmd(sock, {"cmd": "set_input", "buttons": args[1]}))
    elif cmd == "clear_input":
        return pretty_json(send_cmd(sock, {"cmd": "clear_input"}))
    elif cmd == "quit":
        return pretty_json(send_cmd(sock, {"cmd": "quit"}))
    else:
        # Try sending as raw command
        return pretty_json(send_cmd(sock, {"cmd": cmd}))


def main():
    host = DEFAULT_HOST
    port = DEFAULT_PORT

    # Parse --host / --port before command args
    args = sys.argv[1:]
    while args and args[0].startswith("--"):
        if args[0] == "--port" and len(args) > 1:
            port = int(args[1])
            args = args[2:]
        elif args[0] == "--host" and len(args) > 1:
            host = args[1]
            args = args[2:]
        else:
            break

    try:
        sock = connect(host, port)
    except ConnectionRefusedError:
        print(f"Cannot connect to {host}:{port} -- is the game running?")
        sys.exit(1)

    if args:
        result = run_command(sock, args)
        if result:
            print(result)
        sock.close()
        return

    # Interactive REPL
    print(f"Connected to GB debug server at {host}:{port}")
    print("Type 'help' for commands, Ctrl-C to exit\n")
    try:
        while True:
            try:
                line = input("gb> ").strip()
            except EOFError:
                break
            if not line:
                continue
            if line.lower() == "help":
                print(__doc__)
                continue
            args = line.split()
            result = run_command(sock, args)
            if result:
                print(result)
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()


if __name__ == "__main__":
    main()
