#!/usr/bin/env python3
"""Makes a PSP-sized assets.pak from a full OptiCraft assets.pak.

The PS2 pak is ~210 MB, almost all of it raw PCM music that the PSP build does not
play yet and that the Memory Stick (about 1.3 MB/s read) would stream badly. This
copies everything else into a new pak the same PakArchive reader opens.

Usage:  make_psp_pak.py <full assets.pak> <output assets.pak>
        [--keep-music]   keep the .pcm music/streaming entries
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_pak  # same format writer the other platforms use

DROP_PREFIXES = ("irx/",)                       # PS2 IOP modules, useless on PSP
MUSIC_PREFIXES = ("resources/streaming/", "resources/music/", "resources/newmusic/")


def read_entries(path):
    with open(path, "rb") as f:
        magic, ver, count, table_off, names_off, names_bytes, align, _ = struct.unpack(
            ">4sIIIIIII", f.read(32))
        if magic != b"MCPK" or ver != 1:
            raise SystemExit("not an MCPK v1 pak: %s" % path)
        f.seek(table_off)
        table = [struct.unpack(">IIII", f.read(16)) for _ in range(count)]
        f.seek(names_off)
        blob = f.read(names_bytes)
        for _, name_off, data_off, size in table:
            key = blob[name_off:blob.find(b"\0", name_off)].decode("utf-8")
            yield key, data_off, size


def main(argv):
    keep_music = "--keep-music" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args

    kept = dropped = dropped_bytes = 0
    with tempfile.TemporaryDirectory() as tmp, open(src, "rb") as f:
        for key, off, size in read_entries(src):
            low = key.lower()
            if low.startswith(DROP_PREFIXES) or (
                    not keep_music and (low.startswith(MUSIC_PREFIXES) or low.endswith(".pcm"))):
                dropped += 1
                dropped_bytes += size
                continue
            out = os.path.join(tmp, key.replace("/", os.sep))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            f.seek(off)
            with open(out, "wb") as o:
                o.write(f.read(size))
            kept += 1
        count, total = make_pak.build(tmp, dst)
    print("kept %d entries, dropped %d (%.1f MB)" % (kept, dropped, dropped_bytes / 1e6))
    print("%s: %d entries, %.1f MB" % (dst, count, total / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
