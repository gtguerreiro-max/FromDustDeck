"""Lists seeded function starts (tools/hints) that are also jump-table targets inside another
function, a sign that a hint split a switch statement into separate functions.

    python -I tools/seed_vs_switch.py
"""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
for hints_file in sorted((root / "tools" / "hints").glob("*.txt")):
    module = hints_file.stem
    hints = {int(l.split()[0], 16) for l in hints_file.read_text().splitlines() if l.strip()}
    gen = root / "generated" / module
    switch_targets = {}
    for cpp in gen.glob("*_recomp.*.cpp"):
        fn = None
        in_switch = False
        for line in cpp.read_text(errors="replace").splitlines():
            m = re.match(r"DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)", line)
            if m:
                fn = m.group(1)
            if "switch (" in line:
                in_switch = True
            if in_switch:
                for t in re.findall(r"(?:goto loc_|sub_)([0-9A-F]{8})", line):
                    switch_targets.setdefault(int(t, 16), fn)
                if line.strip() == "}" or line.startswith("\tdefault:"):
                    in_switch = False
    hit = sorted(a for a in hints if a in switch_targets and f"sub_{a:08X}" != switch_targets[a])
    print(f"{module}: {len(hints)} hints, {len(switch_targets)} switch targets, {len(hit)} hints are "
          f"switch targets of another function")
    for a in hit[:8]:
        print(f"   0x{a:08X} in {switch_targets[a]}")
