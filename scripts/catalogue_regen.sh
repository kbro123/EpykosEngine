#!/usr/bin/env bash
# scripts/catalogue_regen.sh — M4/C1's catalogue regen check (PROBLEM.md §7, DESIGN.md §7 tier 1).
#
# Builds tools/catalogue's generator (the `release` preset; the generator's own output depends
# only on IR STRUCTURE — signature.hpp deliberately excludes row counts and values, so which
# preset built the *generator* cannot change what it emits), runs it against the reference
# workloads (the Stage A tape, the M1 book — both seeded, docs/WORKLOADS.md) and overwrites
# src/catalogue/generated/{kernels_e0.cpp,registry.cpp}. With --check (the CI mode), fails if that
# changes anything the repository does not already have checked in: a byte-for-byte reproduction
# is the whole point of a value-independent Signature (signature.hpp's own file header).
#
# Usage: scripts/catalogue_regen.sh [--check] [--no-build]
#   --check      after regenerating, `git diff --exit-code` the two generated files; exit 1 if
#                they differ from what is committed (CI's own invocation)
#   --no-build   skip configure + build (the release preset must already be built)
# Exit status: 0 regenerated (and, with --check, unchanged); 1 --check found a diff; 2 setup error.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
PRESET=release
BUILD_DIR="$ROOT/build/$PRESET"
OUT_DIR="$ROOT/src/catalogue/generated"
BUILD=1
CHECK=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) CHECK=1 ;;
    --no-build) BUILD=0 ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "catalogue_regen: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

cd "$ROOT"

if [[ $BUILD -eq 1 ]]; then
  echo "== configure + build preset '$PRESET' (target catalogue_generate)"
  cmake --preset "$PRESET"
  cmake --build --preset "$PRESET" --target catalogue_generate
fi

GEN="$BUILD_DIR/tools/catalogue/catalogue_generate"
if [[ ! -x "$GEN" ]]; then
  echo "catalogue_regen: $GEN not found (build it, or drop --no-build)" >&2
  exit 2
fi

echo "== running the generator against the reference workloads (this records the full Stage A tape: ~seconds)"
"$GEN" "$OUT_DIR"

if [[ $CHECK -eq 1 ]]; then
  echo "== git diff --exit-code on the generated files"
  if ! git -C "$ROOT" diff --exit-code -- "$OUT_DIR"; then
    echo "catalogue_regen: regenerating changed src/catalogue/generated/ — commit the regenerated files (or the generator/signature change that caused it)" >&2
    exit 1
  fi
  echo "   unchanged"
fi
