"""Summarises the cell counts of the whole design from a Yosys log (the
last statistics block, which covers the full hierarchy)."""
import re
import sys

log = open(sys.argv[1]).read()
block = log[log.rindex("Number of cells:"):]
groups = {}
for name, count in re.findall(r"^\s+(\w+)\s+(\d+)\s*$", block, re.M):
    key = re.sub(r"\d+$", "", name) if name.startswith("LUT") else name
    key = {"FDRE": "flip-flops", "FDSE": "flip-flops", "FDCE": "flip-flops", "FDPE": "flip-flops",
           "LUT": "LUTs", "DSP48A1": "DSP48A1", "RAMB16BWER": "RAMB16", "RAMB8BWER": "RAMB8 (half a RAMB16)",
           "RAM32M": "LUT RAM (RAM32M)", "RAM64M": "LUT RAM (RAM64M)"}.get(key)
    if key:
        groups[key] = groups.get(key, 0) + int(count)
for k in sorted(groups):
    print("  %-24s %d" % (k, groups[k]))
