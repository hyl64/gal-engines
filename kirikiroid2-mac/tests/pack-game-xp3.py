#!/usr/bin/env python3
"""Pack a KAG game folder into an XP3 archive, the form real games ship in.

    pack-game-xp3.py <game-folder> <out.xp3>

Every file with an engine-visible extension is stored under its path
relative to the game folder (the same names the game's own scripts ask
for), so `krkrrun out.xp3` boots the game exactly like the released form.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from make_xp3 import build  # the same writer the round-trip test uses

EXTENSIONS = {
    ".tjs", ".ks", ".png", ".jpg", ".jpeg", ".tlg", ".ogg", ".wav",
    ".asd", ".bmp",
}
SKIP_PARTS = {"savedata"}  # save slots are per-run state, not game data


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    root = Path(argv[1])
    out = Path(argv[2])
    entries = []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if any(part in SKIP_PARTS for part in p.relative_to(root).parts):
            continue
        if p.name.startswith("data") and p.suffix == ".ksd":
            continue
        if p.name == "krkr.console.log":
            continue
        if p.suffix.lower() not in EXTENSIONS:
            continue
        name = p.relative_to(root).as_posix()
        entries.append((name, p.read_bytes()))
    blob = build(entries)
    out.write_bytes(blob)
    print(f"wrote {out}: {len(entries)} file(s), {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
