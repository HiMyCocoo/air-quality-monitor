#!/usr/bin/env python3

from pathlib import Path
import gzip
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gzip_asset.py <source> <destination>", file=sys.stderr)
        return 1

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(gzip.compress(src.read_bytes(), compresslevel=9, mtime=0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
