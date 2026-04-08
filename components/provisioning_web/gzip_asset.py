#!/usr/bin/env python3

from pathlib import Path
import gzip
import re
import sys


def minify_html(content: str) -> str:
    content = re.sub(r"<!--.*?-->", "", content, flags=re.DOTALL)
    lines = []
    for raw_line in content.splitlines():
        line = raw_line.strip()
        if line:
            lines.append(line)
    return re.sub(r">\s+<", "><", "\n".join(lines))


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gzip_asset.py <source> <destination>", file=sys.stderr)
        return 1

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    dst.parent.mkdir(parents=True, exist_ok=True)
    minified = minify_html(src.read_text(encoding="utf-8")).encode("utf-8")
    dst.write_bytes(gzip.compress(minified, compresslevel=9, mtime=0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
