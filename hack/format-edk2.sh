#!/usr/bin/env bash
# Format the EDK2/TianoCore C sources carried by this repo's Yocto layer
# with the official TianoCore UncrustifyCheck tooling
# (edk2 .pytool/Plugin/UncrustifyCheck -- see its Readme.md):
#
#   binary : tianocore/uncrustify fork, release 73.0.11 -- the exact pin
#            (URL + sha256) from edk2's uncrustify_ext_dep.yaml; fetched
#            once into ~/.cache and reused. UNCRUSTIFY=/path overrides.
#   config : hack/uncrustify.cfg, a verbatim copy of edk2
#            .pytool/Plugin/UncrustifyCheck/uncrustify.cfg at the SRCREV the
#            edk2 recipe builds (2970e5699). One deviation applied on top:
#            'newlines = lf', because this repo stores LF (.editorconfig);
#            --crlf drops it for code headed into an upstream edk2 PR.
#
#   hack/format-edk2.sh [--check] [--crlf] [paths...]
#
#   --check      report files that need formatting and exit 1; change nothing
#   --crlf       keep upstream's CRLF line-ending rule instead of LF
#   --update-cfg refresh hack/uncrustify.cfg from the edk2 checkout under
#                build/tmp/work (run after bumping the edk2 recipe SRCREV)
#   paths...     format only these files/directories; default: every
#                meta-rpi5-uefi/recipes-bsp/edk2*/files tree (*.c *.h *.cpp,
#                same extensions as the upstream plugin)
#
# The OP-TEE/TF-A sources in the layer follow kernel style, not EDK2 style,
# and are deliberately out of scope.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG="${REPO_ROOT}/hack/uncrustify.cfg"

# Pin from edk2 .pytool/Plugin/UncrustifyCheck/uncrustify_ext_dep.yaml.
UNCRUSTIFY_VERSION="73.0.11"
UNCRUSTIFY_URL="https://github.com/tianocore/uncrustify/releases/download/${UNCRUSTIFY_VERSION}/uncrustify-release.zip"
UNCRUSTIFY_SHA256="b16ee03de6551a7aa547626bd895f1f91d04467b6167334f41a2acbef4617c9b"
CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/tianocore-uncrustify/${UNCRUSTIFY_VERSION}"

CHECK=0
CRLF=0
PATHS=()

die()  { echo "format-edk2: error: $*" >&2; exit 1; }
warn() { echo "format-edk2: warning: $*" >&2; }

usage() {
    awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' \
        "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

update_cfg() {
    local src
    src=$(find "${REPO_ROOT}/build/tmp/work" -type f \
               -path '*/edk2/.pytool/Plugin/UncrustifyCheck/uncrustify.cfg' \
               2>/dev/null | head -n1)
    [ -n "${src}" ] || die "no edk2 checkout under build/tmp/work -- run 'kas build' (or at least the edk2 fetch task) first"
    cp "${src}" "${CFG}"
    echo "refreshed ${CFG#"${REPO_ROOT}"/} from ${src#"${REPO_ROOT}"/}"
    echo "review 'git diff hack/uncrustify.cfg', check the version pin in this script against"
    echo "the checkout's uncrustify_ext_dep.yaml, and commit"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --check)      CHECK=1 ;;
        --crlf)       CRLF=1 ;;
        --update-cfg) update_cfg ;;
        -h|--help)    usage ;;
        -*)           usage 1 ;;
        *)            PATHS+=("$1") ;;
    esac
    shift
done

[ -r "${CFG}" ] || die "config not found: ${CFG}"

# ---------------------------------------------------------------------------
# Locate (or fetch) the TianoCore uncrustify fork. Plain distro uncrustify is
# a last resort: the fork carries EDK2-specific options, so results and
# option coverage differ from upstream CI.
# ---------------------------------------------------------------------------
host_zip_dir() {
    case "$(uname -s)/$(uname -m)" in
        Linux/x86_64)          echo "Linux-x86" ;;
        Linux/aarch64)         echo "Linux-ARM" ;;
        Darwin/x86_64|Darwin/arm64) echo "MacOs-x86" ;;
        *)                     return 1 ;;
    esac
}

fetch_uncrustify() {
    local dir zip tmp
    dir=$(host_zip_dir) || { warn "no prebuilt tianocore uncrustify for $(uname -s)/$(uname -m)"; return 1; }
    command -v curl  >/dev/null 2>&1 || { warn "curl not found, cannot fetch uncrustify"; return 1; }
    command -v unzip >/dev/null 2>&1 || { warn "unzip not found, cannot fetch uncrustify"; return 1; }
    tmp=$(mktemp -d) || return 1
    zip="${tmp}/uncrustify-release.zip"
    echo "fetching tianocore uncrustify ${UNCRUSTIFY_VERSION} (one-time, cached in ${CACHE_DIR})" >&2
    if ! curl -fsSL -o "${zip}" "${UNCRUSTIFY_URL}"; then
        warn "download failed: ${UNCRUSTIFY_URL}"
        rm -rf "${tmp}"; return 1
    fi
    if ! echo "${UNCRUSTIFY_SHA256}  ${zip}" | sha256sum -c --quiet -; then
        warn "sha256 mismatch on ${UNCRUSTIFY_URL} -- refusing to use it"
        rm -rf "${tmp}"; return 1
    fi
    unzip -q -j -d "${tmp}" "${zip}" "${dir}/uncrustify" || { rm -rf "${tmp}"; return 1; }
    mkdir -p "${CACHE_DIR}"
    install -m 0755 "${tmp}/uncrustify" "${CACHE_DIR}/uncrustify"
    rm -rf "${tmp}"
}

if [ -n "${UNCRUSTIFY:-}" ]; then
    BIN="${UNCRUSTIFY}"
    [ -x "${BIN}" ] || die "\$UNCRUSTIFY (${BIN}) is not executable"
elif [ -x "${CACHE_DIR}/uncrustify" ] || fetch_uncrustify; then
    BIN="${CACHE_DIR}/uncrustify"
elif command -v uncrustify >/dev/null 2>&1; then
    BIN=$(command -v uncrustify)
    warn "using system uncrustify ($("${BIN}" --version 2>/dev/null)); the TianoCore fork"
    warn "(${UNCRUSTIFY_VERSION}) is what edk2 CI runs -- expect option warnings and format drift"
else
    die "no uncrustify available: could not fetch the TianoCore build and none in PATH
set UNCRUSTIFY=/path/to/uncrustify, or install curl+unzip and re-run"
fi

# ---------------------------------------------------------------------------
# Collect the files to format.
# ---------------------------------------------------------------------------
TMPDIR_FMT=$(mktemp -d)
trap 'rm -rf "${TMPDIR_FMT}"' EXIT
FILE_LIST="${TMPDIR_FMT}/files.txt"

collect() {
    local p
    if [ "${#PATHS[@]}" -eq 0 ]; then
        find "${REPO_ROOT}/meta-rpi5-uefi/recipes-bsp/"edk2*/files -type f \
             \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) 2>/dev/null
        return 0
    fi
    for p in "${PATHS[@]}"; do
        if [ -d "${p}" ]; then
            find "${p}" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \)
        elif [ -f "${p}" ]; then
            echo "${p}"
        else
            die "no such file or directory: ${p}"
        fi
    done
}
collect | sort -u > "${FILE_LIST}"
NFILES=$(wc -l < "${FILE_LIST}")
[ "${NFILES}" -gt 0 ] || die "no .c/.h/.cpp files found to format"

# Effective config: the vendored upstream file, plus this repo's line-ending
# override appended (uncrustify: the last occurrence of an option wins).
EFF_CFG="${TMPDIR_FMT}/uncrustify.cfg"
cp "${CFG}" "${EFF_CFG}"
if [ "${CRLF}" != "1" ]; then
    printf '\nnewlines = lf\n' >> "${EFF_CFG}"
fi

# ---------------------------------------------------------------------------
# Run it. Same invocation as the upstream plugin: -c cfg -F filelist, with
# --replace --no-backup --if-changed to format, --check to only verify.
# ---------------------------------------------------------------------------
if [ "${CHECK}" = "1" ]; then
    OUT="${TMPDIR_FMT}/check.out"
    if "${BIN}" -c "${EFF_CFG}" -F "${FILE_LIST}" --check -q > "${OUT}" 2>&1; then
        echo "format-edk2: ${NFILES} files clean (TianoCore uncrustify ${UNCRUSTIFY_VERSION})"
    else
        grep '^FAIL: ' "${OUT}" | sed "s|^FAIL: |needs formatting: |; s|${REPO_ROOT}/||" >&2
        NFAIL=$(grep -c '^FAIL: ' "${OUT}" || true)
        echo "format-edk2: ${NFAIL:-?} of ${NFILES} files need formatting -- run 'make format-edk2'" >&2
        exit 1
    fi
else
    "${BIN}" -c "${EFF_CFG}" -F "${FILE_LIST}" --replace --no-backup --if-changed -q
    echo "format-edk2: formatted ${NFILES} files in place (TianoCore uncrustify ${UNCRUSTIFY_VERSION})"
    echo "review with: git diff -- meta-rpi5-uefi/recipes-bsp/edk2*/files"
fi
