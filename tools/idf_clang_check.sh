#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"

build_dir="${IDF_CLANG_CHECK_BUILD_DIR:-${project_dir}/.cache/clang-check-build}"
output_dir="${IDF_CLANG_CHECK_OUTPUT_DIR:-${project_dir}/.cache/checks}"
sdkconfig_src="${IDF_CLANG_CHECK_SDKCONFIG_SOURCE:-${project_dir}/sdkconfig}"
sdkconfig="${IDF_CLANG_CHECK_SDKCONFIG:-${project_dir}/.cache/clang-check-sdkconfig}"
warnings_src="${project_dir}/warnings.txt"
warnings_dst="${IDF_CLANG_CHECK_WARNINGS:-${output_dir}/warnings.txt}"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found; source the ESP-IDF export script before running clang-check." >&2
    exit 127
fi

mkdir -p "${output_dir}"
rm -f "${warnings_src}"

if [[ -f "${sdkconfig_src}" ]]; then
    cp "${sdkconfig_src}" "${sdkconfig}"
fi

IDF_TOOLCHAIN=clang idf.py \
    -D "SDKCONFIG=${sdkconfig}" \
    -B "${build_dir}" \
    -C "${project_dir}" \
    reconfigure
configure_status=$?
if [[ "${configure_status}" -ne 0 ]]; then
    exit "${configure_status}"
fi

IDF_TOOLCHAIN=clang idf.py \
    -B "${build_dir}" \
    -C "${project_dir}" \
    clang-check \
    --exclude-paths managed_components \
    --exclude-paths build \
    --exclude-paths .cache \
    --exclude-paths components/platform/include/platform_config_local.h \
    "$@"
check_status=$?

if [[ -f "${warnings_src}" ]]; then
    mv -f "${warnings_src}" "${warnings_dst}"
    echo "clang-check warnings: ${warnings_dst}"
fi

exit "${check_status}"
