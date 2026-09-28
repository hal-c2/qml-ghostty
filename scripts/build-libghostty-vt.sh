#!/usr/bin/env bash
# Builds libghostty-vt for the host from the Ghostty revision pinned in
# libghostty-vt.version and installs headers and libraries into
# third_party/libghostty-vt. Keep the pin equal to hal-c2's
# native/libghostty-vt/VERSION so both sides share one C ABI.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REVISION="${GHOSTTY_REVISION:-$(tr -d '[:space:]' < "${ROOT}/libghostty-vt.version")}"
CACHE="${QML_GHOSTTY_CACHE:-${XDG_CACHE_HOME:-${HOME}/.cache}/qml-ghostty}"
SOURCE="${GHOSTTY_SOURCE_DIR:-${CACHE}/ghostty-${REVISION:0:8}}"
ZIG_VERSION="${GHOSTTY_ZIG_VERSION:-0.15.2}"
ZIG="${GHOSTTY_ZIG:-}"
PREFIX="${ROOT}/third_party/libghostty-vt"

log() { printf '[libghostty-vt] %s\n' "$*"; }
die() { printf '[libghostty-vt] error: %s\n' "$*" >&2; exit 1; }

if [[ -z "${ZIG}" ]]; then
  if command -v zig >/dev/null 2>&1 && [[ "$(zig version 2>/dev/null)" == "${ZIG_VERSION}" ]]; then
    ZIG="$(command -v zig)"
  else
    arch="$(uname -m)"; os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    [[ "${arch}" == arm64 ]] && arch=aarch64
    [[ "${os}" == darwin ]] && os=macos
    ZIG="${CACHE}/zig-${ZIG_VERSION}/zig"
    if [[ ! -x "${ZIG}" ]]; then
      log "downloading Zig ${ZIG_VERSION}"
      mkdir -p "${CACHE}/zig-${ZIG_VERSION}"
      curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-${arch}-${os}-${ZIG_VERSION}.tar.xz" \
        | tar -xJ --strip-components=1 -C "${CACHE}/zig-${ZIG_VERSION}"
    fi
  fi
fi

if [[ ! -d "${SOURCE}/.git" ]]; then
  log "cloning Ghostty ${REVISION}"
  git clone --filter=blob:none --no-checkout https://github.com/ghostty-org/ghostty.git "${SOURCE}"
  git -C "${SOURCE}" fetch --depth=1 origin "${REVISION}"
  git -C "${SOURCE}" checkout --detach "${REVISION}"
fi
[[ "$(git -C "${SOURCE}" rev-parse HEAD)" == "${REVISION}" ]] || die "expected Ghostty ${REVISION} in ${SOURCE}"

log "building into ${PREFIX}"
rm -rf "${PREFIX}"
(cd "${SOURCE}" && "${ZIG}" build -Demit-lib-vt -Doptimize=ReleaseFast -p "${PREFIX}")
cp "${SOURCE}/LICENSE" "${PREFIX}/LICENSE"
printf '%s\n' "${REVISION}" > "${PREFIX}/VERSION"
log "done"
