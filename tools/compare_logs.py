"""Compares two Paula register-write logs and describes the first difference.

    python3 tools/compare_logs.py theirs.log ours.log
"""
import re
import sys

HEADER = re.compile(r"tick (\d+) pos (\d+) row (\d+)")
WRITE = re.compile(r"\s*AUD(\d)(\w+) (\S+)")


def _where(lines, index):
    """The tick header in force at lines[index]."""
    for i in range(min(index, len(lines) - 1), -1, -1):
        m = HEADER.match(lines[i])
        if m:
            return "tick %s, position %s, row %s" % m.groups()
    return "before the first tick"


def first_difference(theirs, ours):
    """None if the logs match, else a one-paragraph description."""
    for i in range(max(len(theirs), len(ours))):
        a = theirs[i] if i < len(theirs) else None
        b = ours[i] if i < len(ours) else None
        if a == b:
            continue
        where = _where(theirs if a is not None else ours, i)
        ma, mb = (WRITE.match(a) if a else None), (WRITE.match(b) if b else None)
        if ma and mb and ma.group(1, 2) == mb.group(1, 2):
            return "%s: voice %d %s: pt2-clone %s, ours %s" % (
                where, int(ma.group(1)) + 1, ma.group(2), ma.group(3), mb.group(3))
        return "%s (log line %d): pt2-clone: %s; ours: %s" % (
            where, i + 1, (a or "(nothing)").strip(), (b or "(nothing)").strip())
    return None


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        theirs = f.read().splitlines()
    with open(argv[2]) as f:
        ours = f.read().splitlines()
    diff = first_difference(theirs, ours)
    print(diff or "logs match")
    return 1 if diff else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
