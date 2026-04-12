#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
elif command -v getconf >/dev/null 2>&1; then
    JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
else
    JOBS="${JOBS:-4}"
fi

needs_root=1
if [[ -e "${INSTALL_PREFIX}" ]]; then
    if [[ -w "${INSTALL_PREFIX}" ]]; then
        needs_root=0
    fi
elif [[ -w "$(dirname "${INSTALL_PREFIX}")" ]]; then
    needs_root=0
fi

if [[ "${EUID}" -eq 0 ]]; then
    ROOT_CMD=()
elif [[ "${needs_root}" -eq 0 ]]; then
    ROOT_CMD=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT_CMD=(sudo)
else
    echo "error: root privileges are required for install/uninstall, but sudo is not available." >&2
    exit 1
fi

run_root() {
    "${ROOT_CMD[@]}" "$@"
}

cache_var() {
    local key="$1"
    local cache_file="${BUILD_DIR}/CMakeCache.txt"
    if [[ -f "${cache_file}" ]]; then
        sed -n "s/^${key}:[^=]*=//p" "${cache_file}" | head -n 1
    fi
}

remove_manifest_entries_under_prefix() {
    local manifest="${BUILD_DIR}/install_manifest.txt"
    if [[ ! -f "${manifest}" ]]; then
        return
    fi

    echo "Removing previously installed files recorded in ${manifest} under ${INSTALL_PREFIX} ..."
    while IFS= read -r installed_path; do
        [[ -z "${installed_path}" ]] && continue
        case "${installed_path}" in
            "${INSTALL_PREFIX}"/*)
                run_root rm -f "${installed_path}"
                ;;
        esac
    done < "${manifest}"
}

remove_legacy_install_layout() {
    local includedir_rel="$1"
    local libdir_rel="$2"
    local include_root="${INSTALL_PREFIX}/${includedir_rel}"
    local lib_root="${INSTALL_PREFIX}/${libdir_rel}"

    echo "Removing legacy DeltaDB installation under ${INSTALL_PREFIX} ..."
    run_root rm -rf "${include_root}/deltadb"
    run_root rm -rf "${lib_root}/cmake/DeltaDB"

    run_root rm -f \
        "${lib_root}/libdb.a" \
        "${lib_root}/libtable.a" \
        "${lib_root}/libwal.a" \
        "${lib_root}/libutils.a"

    # Older revisions may have installed archives with a project-specific prefix.
    run_root sh -c "rm -f '${lib_root}'/libDeltaDB*.a"
}

configure() {
    echo "Configuring build in ${BUILD_DIR} ..."
    echo "  app:   OFF"
    echo "  tests: OFF"
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -DDELTA_DB_BUILD_APP=OFF \
        -DDELTA_DB_BUILD_TESTS=OFF
}

build() {
    echo "Building DeltaDB ..."
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
}

install_project() {
    echo "Installing DeltaDB to ${INSTALL_PREFIX} ..."
    run_root cmake --install "${BUILD_DIR}"
}

main() {
    configure

    local includedir_rel libdir_rel
    includedir_rel="$(cache_var CMAKE_INSTALL_INCLUDEDIR)"
    libdir_rel="$(cache_var CMAKE_INSTALL_LIBDIR)"
    includedir_rel="${includedir_rel:-include}"
    libdir_rel="${libdir_rel:-lib}"

    remove_manifest_entries_under_prefix
    remove_legacy_install_layout "${includedir_rel}" "${libdir_rel}"

    build
    install_project

    echo "DeltaDB has been reinstalled to ${INSTALL_PREFIX}."
}

main "$@"
