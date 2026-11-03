#!/usr/bin/env bash
# Puts Espressif's standalone esptool next to the app as a Tauri sidecar, for one target triple, and copies its licence.
# The archives are pinned by SHA-256, so a changed download stops the build.
# usage: scripts/fetch-esptool.sh <rust target triple>
set -euo pipefail
VERSION=v5.6.0
triple="$1"
case "$triple" in
  x86_64-unknown-linux-gnu) asset=linux-amd64.tar.gz; sum=1590723cfa9675983f49d0bf2895b8507f8d2879082e51bb7ef02a8bca88d354; exe=esptool ;;
  x86_64-apple-darwin)      asset=macos-amd64.tar.gz; sum=23921776a7df75f2e6f3ca10a604d74db4661e1b8a0ebb89bdddcd2d26f37055; exe=esptool ;;
  aarch64-apple-darwin)     asset=macos-arm64.tar.gz; sum=08a58829a3f14bb08f2e39f4cad48dcb14b7000384268e7f06689836332d6e25; exe=esptool ;;
  x86_64-pc-windows-msvc)   asset=windows-amd64.zip;  sum=d55a96fbeb97fd4cb05bd93f201e5c8f6518b7bfcde2fc6966fd0190fd891629; exe=esptool.exe ;;
  *) echo "no esptool build for $triple" >&2; exit 1 ;;
esac
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
curl -sSfL --retry 3 -o "$tmp/archive" "https://github.com/espressif/esptool/releases/download/$VERSION/esptool-$VERSION-$asset"
got="$( (command -v sha256sum >/dev/null && sha256sum "$tmp/archive" || shasum -a 256 "$tmp/archive") | cut -d' ' -f1)"
[ "$got" = "$sum" ] || { echo "esptool archive checksum mismatch: $got" >&2; exit 1; }
case "$asset" in
  *.zip) (cd "$tmp" && unzip -q archive) ;;
  *)     tar xzf "$tmp/archive" -C "$tmp" ;;
esac
src="$(find "$tmp" -type f -name "$exe" | head -n1)"
lic="$(find "$tmp" -type f -name LICENSE | head -n1)"
suffix=""; [ "$exe" = "esptool.exe" ] && suffix=".exe"
mkdir -p "$here/src-tauri/binaries" "$here/src-tauri/resources"
cp "$src" "$here/src-tauri/binaries/esptool-$triple$suffix"
chmod +x "$here/src-tauri/binaries/esptool-$triple$suffix"
cp "$lic" "$here/src-tauri/resources/esptool-LICENSE.txt"
echo "esptool $VERSION for $triple ready"
