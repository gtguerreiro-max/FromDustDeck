#!/usr/bin/env python3
"""Lists and adds Steam's non-Steam game shortcuts (userdata/<user>/config/shortcuts.vdf).

  steam_shortcuts.py list
  steam_shortcuts.py add NAME EXE [--options '"--flag" ...'] [--start-dir DIR] [--icon PNG]
                     [--desktop FILE] [--art-from NAME] [--art-dir DIR]
  steam_shortcuts.py remove NAME...

add updates the shortcut of the same NAME (the options given), or appends one with the app ID Steam gives it (the
CRC-32 of the quoted EXE and NAME, high bit set), so Steam keeps its play time and artwork.
--art-from copies another shortcut's artwork (config/grid/<appid>*) to it. --art-dir copies
portrait.png, grid.png, hero.png and logo.png from DIR to it, and uses DIR/icon.png as its icon. Steam holds the file
in memory and writes it back when it exits, so this refuses while Steam runs. The file is binary
VDF: 0x00 a map, 0x01 a string, 0x02 an int32, 0x08 the end of a map, keys NUL-terminated.
"""

import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

MAP, STRING, INT32, END = 0, 1, 2, 8


def parse(data: bytes) -> dict:
    pos = 0

    def cstr() -> str:
        nonlocal pos
        end = data.index(b"\0", pos)
        s = data[pos:end].decode()
        pos = end + 1
        return s

    def mapping() -> dict:
        nonlocal pos
        out = {}
        while (kind := data[pos]) != END:
            pos += 1
            key = cstr()
            if kind == MAP:
                out[key] = mapping()
            elif kind == STRING:
                out[key] = cstr()
            elif kind == INT32:
                out[key] = struct.unpack_from("<i", data, pos)[0]
                pos += 4
            else:
                sys.exit(f"shortcuts.vdf: value type {kind} at {pos - 1}")
        pos += 1
        return out

    root = mapping()
    if pos != len(data):
        sys.exit(f"shortcuts.vdf: {len(data) - pos} bytes after the root")
    return root


def serialize(root: dict) -> bytes:
    out = bytearray()

    def mapping(m: dict) -> None:
        for key, value in m.items():
            if isinstance(value, dict):
                out.extend(bytes([MAP]) + key.encode() + b"\0")
                mapping(value)
            elif isinstance(value, int):
                out.extend(bytes([INT32]) + key.encode() + b"\0" + struct.pack("<i", value))
            else:
                out.extend(bytes([STRING]) + key.encode() + b"\0" + value.encode() + b"\0")
        out.append(END)

    mapping(root)
    return bytes(out)


def app_id(exe: str, name: str) -> int:
    """The signed int32 Steam stores: CRC-32 of exe + name with the high bit set."""
    return struct.unpack("<i", struct.pack("<I", zlib.crc32((exe + name).encode()) | 0x80000000))[0]


def unsigned(appid: int) -> int:
    return appid & 0xFFFFFFFF


def exe_of(entry: dict) -> str:
    return entry.get("exe", entry.get("Exe", ""))


def config_dir() -> str:
    dirs = glob.glob(os.path.expanduser("~/.local/share/Steam/userdata/*/config"))
    dirs = [d for d in dirs if not d.split(os.sep)[-2] == "0"]
    if len(dirs) != 1:
        sys.exit(f"want one Steam user's config, found {len(dirs)}")
    return dirs[0]


def steam_running() -> bool:
    return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0


def write(path: str, root: dict) -> None:
    data = serialize(root)
    if os.path.exists(path):
        if open(path, "rb").read() == data:
            return
        shutil.copyfile(path, f"{path}.bak-{time.strftime('%Y%m%d-%H%M%S')}")
    with open(path + ".tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".tmp", path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    remove = sub.add_parser("remove")
    remove.add_argument("name", nargs="+")
    add = sub.add_parser("add")
    add.add_argument("name")
    add.add_argument("exe")
    add.add_argument("--options")
    add.add_argument("--start-dir")
    add.add_argument("--icon")
    add.add_argument("--desktop")
    add.add_argument("--art-from")
    add.add_argument("--art-dir")
    args = ap.parse_args()

    config = config_dir()
    path = os.path.join(config, "shortcuts.vdf")
    root = parse(open(path, "rb").read()) if os.path.exists(path) else {"shortcuts": {}}
    shortcuts = root["shortcuts"]
    if args.cmd == "list":
        for entry in shortcuts.values():
            print(f"{unsigned(entry['appid']):10d}  {entry['AppName']}  {exe_of(entry)} "
                  f"{entry.get('LaunchOptions', '')}")
        return

    if steam_running():
        sys.exit("Steam is running and would write its own shortcuts back: quit it first")
    if args.cmd == "remove":
        removed = False
        for name in args.name:
            key = next((k for k, e in shortcuts.items() if e["AppName"] == name), None)
            if key is None:
                print(f"no shortcut named {name}")
            else:
                del shortcuts[key]
                removed = True
        if removed:
            write(path, root)
        return
    exe = f'"{args.exe}"'
    start_dir = args.start_dir or os.path.dirname(args.exe) + "/"
    entry = next((e for e in shortcuts.values() if e["AppName"] == args.name), None)
    if entry is None:
        entry = {"appid": app_id(exe, args.name), "AppName": args.name, "exe": exe,
                 "StartDir": start_dir, "icon": "", "ShortcutPath": "", "LaunchOptions": "",
                 "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0,
                 "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 0,
                 "FlatpakAppID": "", "sortas": "", "tags": {}}
        shortcuts[str(len(shortcuts))] = entry
    # Steam writes the key as "exe"; an entry from an older version of this script may have
    # "Exe", which Steam doesn't read: keep one key, Steam's.
    entry.pop("Exe", None)
    entry.update(exe=exe, StartDir=start_dir)
    for key, value in (("icon", args.icon), ("ShortcutPath", args.desktop),
                       ("LaunchOptions", args.options)):
        if value is not None:
            entry[key] = value

    if args.art_from:
        source = next((e for e in shortcuts.values() if e["AppName"] == args.art_from), None)
        if source is None:
            sys.exit(f"no shortcut named {args.art_from}")
        grid = os.path.join(config, "grid")
        prefix = str(unsigned(source["appid"]))
        for art in glob.glob(os.path.join(grid, prefix + "*")):
            suffix = os.path.basename(art)[len(prefix):]
            shutil.copyfile(art, os.path.join(grid, str(unsigned(entry["appid"])) + suffix))

    if args.art_dir:
        # Steam's names for a shortcut's artwork in config/grid: <appid>p (library capsule,
        # 600x900), <appid> (wide capsule, 920x430), <appid>_hero (1920x620), <appid>_logo.
        grid = os.path.join(config, "grid")
        os.makedirs(grid, exist_ok=True)
        appid = str(unsigned(entry["appid"]))
        for name, suffix in (("portrait", "p"), ("grid", ""), ("hero", "_hero"), ("logo", "_logo")):
            art = os.path.join(args.art_dir, name + ".png")
            if os.path.exists(art):
                shutil.copyfile(art, os.path.join(grid, appid + suffix + ".png"))
        icon = os.path.join(os.path.abspath(args.art_dir), "icon.png")
        if args.icon is None and os.path.exists(icon):
            entry["icon"] = icon

    write(path, root)


if __name__ == "__main__":
    main()
