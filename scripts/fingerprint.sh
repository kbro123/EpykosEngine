#!/usr/bin/env bash
# scripts/fingerprint.sh — machine + toolchain fingerprint (D9, D13).
#
# id = first 12 hex of sha256("<cpu brand>|<physical cores>|<logical cores>|<compiler --version>|<release flags>").
# Prints one JSON line (which also carries the 1-minute load average, NOT part of the id) followed by
# the id. Performance numbers are only compared within one id; results live in bench/results/<id>/.
#
# The flags are those of the release preset: read from build/release/epykos_flags.txt when that tree
# is configured, otherwise the D13 default for this host's architecture (identical on x86-64).
# The compiler is $CXX, else the one recorded in build/release/CMakeCache.txt, else `c++`.
#
# Usage: scripts/fingerprint.sh [--json | --id] [--build-dir DIR]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="$ROOT/build/release"
MODE=both
while [[ $# -gt 0 ]]; do
  case "$1" in
    --json) MODE=json ;;
    --id) MODE=id ;;
    --build-dir) shift; BUILD_DIR="$1" ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "fingerprint: unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Darwin)
    cpu="$(sysctl -n machdep.cpu.brand_string)"
    cores_physical="$(sysctl -n hw.physicalcpu)"
    cores_logical="$(sysctl -n hw.logicalcpu)"
    load1="$(sysctl -n vm.loadavg | awk '{print $2}')"
    ;;
  Linux)
    cpu="$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n1)"
    [[ -n "$cpu" ]] || cpu="$(lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -n1)"
    [[ -n "$cpu" ]] || cpu="unknown"
    cores_logical="$(nproc)"
    cores_physical="$(lscpu -p=CORE,SOCKET 2>/dev/null | grep -v '^#' | sort -u | wc -l | tr -d ' ')"
    [[ "${cores_physical:-0}" -gt 0 ]] || cores_physical="$cores_logical"
    load1="$(cut -d' ' -f1 /proc/loadavg)"
    ;;
  *)
    cpu="unknown"; cores_physical=0; cores_logical=0; load1=0
    ;;
esac

cxx="${CXX:-}"
if [[ -z "$cxx" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Za-z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
fi
[[ -n "$cxx" ]] || cxx=c++
compiler="$("$cxx" --version 2>/dev/null | head -n1 || true)"
[[ -n "$compiler" ]] || compiler="unknown ($cxx)"

if [[ -f "$BUILD_DIR/epykos_flags.txt" ]]; then
  flags="$(head -n1 "$BUILD_DIR/epykos_flags.txt")"
  flags_source="$BUILD_DIR/epykos_flags.txt"
else
  case "$arch" in
    x86_64|amd64) flags="-O3 -march=x86-64-v3 -fno-math-errno" ;;
    *)            flags="-O3 -fno-math-errno" ;;
  esac
  flags_source="default (D13, release preset not configured)"
fi

canonical="$cpu|$cores_physical|$cores_logical|$compiler|$flags"
if command -v sha256sum >/dev/null 2>&1; then
  digest="$(printf '%s' "$canonical" | sha256sum | awk '{print $1}')"
else
  digest="$(printf '%s' "$canonical" | shasum -a 256 | awk '{print $1}')"
fi
id="${digest:0:12}"

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

json="{\"id\":\"$id\",\"cpu\":\"$(json_escape "$cpu")\",\"cores_physical\":$cores_physical,\"cores_logical\":$cores_logical,\"compiler\":\"$(json_escape "$compiler")\",\"flags\":\"$(json_escape "$flags")\",\"os\":\"$(json_escape "$os")\",\"arch\":\"$(json_escape "$arch")\",\"load1\":$load1,\"flags_source\":\"$(json_escape "$flags_source")\"}"

case "$MODE" in
  json) echo "$json" ;;
  id) echo "$id" ;;
  both) echo "$json"; echo "$id" ;;
esac
