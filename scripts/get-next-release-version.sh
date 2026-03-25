#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base_version="$(bash "${repo_root}/scripts/get-project-version.sh")"

if [[ ! "${base_version}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "PROJECT_VER must use semantic version format like 0.1.0" >&2
  exit 1
fi

major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
base_patch="${BASH_REMATCH[3]}"

latest_tag="$(
  git -C "${repo_root}" tag --list "v${major}.${minor}.*" --sort=-v:refname | head -n1
)"

next_patch="${base_patch}"
if [[ -n "${latest_tag}" ]]; then
  latest_version="${latest_tag#v}"
  if [[ "${latest_version}" =~ ^${major}\.${minor}\.([0-9]+)$ ]]; then
    latest_patch="${BASH_REMATCH[1]}"
    candidate_patch=$((latest_patch + 1))
    if (( candidate_patch > next_patch )); then
      next_patch="${candidate_patch}"
    fi
  fi
fi

printf '%s.%s.%s\n' "${major}" "${minor}" "${next_patch}"
