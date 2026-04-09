#!/usr/bin/env python3

from pathlib import Path
import gzip
import re
import sys


INLINE_CSS_PATTERN = re.compile(r"/\*\s*AIRMON_INLINE_CSS:\s*([^*]+?)\s*\*/")
INLINE_JS_PATTERN = re.compile(r"//\s*AIRMON_INLINE_JS:\s*(\S+)")


def inline_asset_references(content: str, asset_dir: Path) -> str:
    def replace_css(match: re.Match[str]) -> str:
        asset = (asset_dir / match.group(1).strip()).resolve()
        return asset.read_text(encoding="utf-8")

    def replace_js(match: re.Match[str]) -> str:
        asset = (asset_dir / match.group(1).strip()).resolve()
        return asset.read_text(encoding="utf-8")

    content = INLINE_CSS_PATTERN.sub(replace_css, content)
    return INLINE_JS_PATTERN.sub(replace_js, content)


def minify_html(content: str) -> str:
    content = re.sub(r"<!--.*?-->", "", content, flags=re.DOTALL)
    lines = []
    for raw_line in content.splitlines():
        line = raw_line.strip()
        if line:
            lines.append(line)
    return re.sub(r">\s+<", "><", "\n".join(lines))


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: gzip_asset.py <source> <destination> [asset-dir]", file=sys.stderr)
        return 1

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    asset_dir = Path(sys.argv[3]) if len(sys.argv) == 4 else src.parent
    dst.parent.mkdir(parents=True, exist_ok=True)
    assembled = inline_asset_references(src.read_text(encoding="utf-8"), asset_dir)
    minified = minify_html(assembled).encode("utf-8")
    dst.write_bytes(gzip.compress(minified, compresslevel=9, mtime=0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
