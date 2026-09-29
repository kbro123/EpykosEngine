#!/usr/bin/env bash
# scripts/bootstrap.sh — fetch the pinned third-party sources (D12) into third_party/ of this checkout.
#
#   Eigen 3.4.0            header-only, double side only (D14); include path only, unused until M4
#   GoogleTest 1.18.0      add_subdirectory (own tests off)
#   Google Benchmark 1.9.5 add_subdirectory (own tests off)
#
# Tarballs are downloaded from the projects' official release hosts, verified against the sha256
# pinned below, and cached under <main checkout>/.cache/downloads so every git worktree shares
# one download. Extraction goes to third_party/<name>/ in the current checkout with a stamp file;
# re-running is a no-op when the stamp matches. Nothing else is installed.
#
# Usage: scripts/bootstrap.sh            (idempotent)
#        EPYKOS_DOWNLOAD_CACHE=/dir scripts/bootstrap.sh   to override the cache location
set -euo pipefail

EIGEN_VERSION=3.4.0
EIGEN_URL="https://gitlab.com/libeigen/eigen/-/archive/${EIGEN_VERSION}/eigen-${EIGEN_VERSION}.tar.gz"
EIGEN_SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72

GTEST_VERSION=1.18.0
GTEST_URL="https://github.com/google/googletest/releases/download/v${GTEST_VERSION}/googletest-${GTEST_VERSION}.tar.gz"
GTEST_SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5

BENCHMARK_VERSION=1.9.5
BENCHMARK_URL="https://github.com/google/benchmark/archive/refs/tags/v${BENCHMARK_VERSION}.tar.gz"
BENCHMARK_SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TP_DIR="$ROOT/third_party"

# Shared download cache: <main checkout>/.cache/downloads (git-common-dir is the main .git even
# from a worktree). Falls back to $ROOT/.cache/downloads outside a git checkout.
if common="$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null)"; then
  [[ "$common" = /* ]] || common="$ROOT/$common"
  CACHE_DIR="$(cd "$common/.." && pwd -P)/.cache/downloads"
else
  CACHE_DIR="$ROOT/.cache/downloads"
fi
CACHE_DIR="${EPYKOS_DOWNLOAD_CACHE:-$CACHE_DIR}"
mkdir -p "$CACHE_DIR" "$TP_DIR"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "bootstrap: need sha256sum or shasum" >&2
    exit 1
  fi
}

# fetch <name> <version> <url> <sha256>: download into the cache unless a verified copy exists.
fetch() {
  local name="$1" version="$2" url="$3" sha="$4"
  local file="$CACHE_DIR/$name-$version.tar.gz"
  if [[ -f "$file" ]] && [[ "$(sha256_of "$file")" == "$sha" ]]; then
    echo "cached   $file"
    return
  fi
  echo "fetching $url"
  local tmp="$file.part"
  rm -f "$tmp"
  curl -fsSL --retry 3 --retry-delay 2 -o "$tmp" "$url"
  local got
  got="$(sha256_of "$tmp")"
  if [[ "$got" != "$sha" ]]; then
    rm -f "$tmp"
    echo "bootstrap: sha256 mismatch for $name $version" >&2
    echo "  expected $sha" >&2
    echo "  got      $got" >&2
    exit 1
  fi
  mv "$tmp" "$file"
  echo "verified $file"
}

# extract <name> <version> <sha256>: unpack into third_party/<name>/ unless the stamp matches.
extract() {
  local name="$1" version="$2" sha="$3"
  local dest="$TP_DIR/$name" stamp="$name $version $sha"
  if [[ -f "$dest/.epykos-stamp" ]] && [[ "$(cat "$dest/.epykos-stamp")" == "$stamp" ]]; then
    echo "present  $dest ($version)"
    return
  fi
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -xzf "$CACHE_DIR/$name-$version.tar.gz" -C "$dest" --strip-components=1
  printf '%s\n' "$stamp" > "$dest/.epykos-stamp"
  echo "unpacked $dest ($version)"
}

fetch eigen      "$EIGEN_VERSION"     "$EIGEN_URL"     "$EIGEN_SHA256"
fetch googletest "$GTEST_VERSION"     "$GTEST_URL"     "$GTEST_SHA256"
fetch benchmark  "$BENCHMARK_VERSION" "$BENCHMARK_URL" "$BENCHMARK_SHA256"

extract eigen      "$EIGEN_VERSION"     "$EIGEN_SHA256"
extract googletest "$GTEST_VERSION"     "$GTEST_SHA256"
extract benchmark  "$BENCHMARK_VERSION" "$BENCHMARK_SHA256"

echo "bootstrap: third_party ready in $TP_DIR (cache: $CACHE_DIR)"
