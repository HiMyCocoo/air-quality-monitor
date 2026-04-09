#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
output_dir="${AIRMON_CHECK_OUTPUT_DIR:-${project_dir}/.cache/checks}"
min_app_free_percent="${AIRMON_MIN_APP_FREE_PERCENT:-20}"

build_log="${output_dir}/build.log"
clang_log="${output_dir}/clang-check.log"
diff_log="${output_dir}/diff-check.log"
py_log="${output_dir}/py-compile.log"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found; source the ESP-IDF export script before running local checks." >&2
    exit 127
fi

mkdir -p "${output_dir}"
cd "${project_dir}" || exit 1

run_logged()
{
    local name="$1"
    local log="$2"
    shift 2

    printf '\n==> %s\n' "${name}"
    "$@" >"${log}" 2>&1
    local status=$?
    if [[ "${status}" -ne 0 ]]; then
        printf '%s failed; see %s\n' "${name}" "${log}" >&2
        tail -n 120 "${log}" >&2
        return "${status}"
    fi
    printf '%s passed; log: %s\n' "${name}" "${log}"
    return 0
}

run_logged "diff whitespace check" "${diff_log}" git diff --check || exit $?
run_logged "ESP-IDF build" "${build_log}" idf.py build || exit $?

size_line="$(grep -E 'binary size 0x[0-9a-fA-F]+ bytes\..*partition is 0x[0-9a-fA-F]+ bytes\..*free\.' "${build_log}" | tail -n 1)"
if [[ -z "${size_line}" ]]; then
    echo "Unable to find app partition size summary in ${build_log}" >&2
    exit 1
fi

printf '\n==> size summary\n%s\n' "${size_line}"
SIZE_LINE="${size_line}" MIN_APP_FREE_PERCENT="${min_app_free_percent}" python3 - <<'PY' || exit $?
import os
import re
import sys

line = os.environ["SIZE_LINE"]
minimum = int(os.environ["MIN_APP_FREE_PERCENT"])
match = re.search(
    r"binary size (0x[0-9a-fA-F]+).*partition is (0x[0-9a-fA-F]+).*\((\d+)%\) free",
    line,
)
if not match:
    print(f"Unable to parse size line: {line}", file=sys.stderr)
    raise SystemExit(1)

image_size = int(match.group(1), 16)
partition_size = int(match.group(2), 16)
free_percent = int(match.group(3))
free_bytes = partition_size - image_size

print(f"app_image=0x{image_size:x} partition=0x{partition_size:x} free=0x{free_bytes:x} ({free_percent}%)")
if free_percent < minimum:
    print(f"App partition free space {free_percent}% is below gate {minimum}%", file=sys.stderr)
    raise SystemExit(1)
PY

run_logged "web asset helper py_compile" "${py_log}" \
    env PYTHONPYCACHEPREFIX="${output_dir}/pycache" \
    python3 -m py_compile components/provisioning_web/gzip_asset.py || exit $?
run_logged "ESP-IDF clang-check" "${clang_log}" "${script_dir}/idf_clang_check.sh" || exit $?

if [[ -f "${output_dir}/warnings.txt" ]]; then
    env_errors="$(rg -n --count-matches '\[clang-diagnostic-error\]|unknown argument|unknown warning option|file not found|fatal error|^error:' "${output_dir}/warnings.txt" "${clang_log}" 2>/dev/null || true)"
    if [[ -n "${env_errors}" ]]; then
        printf '%s\n' "${env_errors}" >&2
        echo "clang-check reported diagnostic/environment errors" >&2
        exit 1
    fi

    warning_count="$(rg -c 'warning:' "${output_dir}/warnings.txt" 2>/dev/null || true)"
    printf '\n==> clang-check warning count\n%s\n' "${warning_count:-0}"
fi

printf '\nLocal checks passed. Outputs: %s\n' "${output_dir}"
