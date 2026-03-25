#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_file="${repo_root}/CMakeLists.txt"

version="$(
  sed -nE 's/^[[:space:]]*set\(PROJECT_VER[[:space:]]+"([^"]+)"\).*/\1/p' "${cmake_file}" | head -n1
)"

if [[ -z "${version}" ]]; then
  echo "Failed to parse PROJECT_VER from ${cmake_file}" >&2
  exit 1
fi

printf '%s\n' "${version}"
