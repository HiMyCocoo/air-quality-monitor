#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <version>" >&2
  exit 1
fi

version="$1"
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Version must use semantic version format like 0.1.0" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_file="${repo_root}/CMakeLists.txt"

perl -0pi -e 's/set\(PROJECT_VER\s+"[^"]+"\)/set(PROJECT_VER "'"${version}"'")/' "${cmake_file}"
