#!/usr/bin/env bash
# Linux arm64 build of the shader translation service (totk_xlate --service), NativeAOT, inside `nix develop`.
# The Vulkan path consumes Ryujinx's SPIR-V directly, so SPIRV-Cross is not linked in.
# usage: games/totk/tools/totk_xlate/publish_linux.sh   -> games/totk/prebuilt/linux-arm64/totk_xlate (+ .sha256)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$REPO/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
dotnet publish "$WORK/xlate/totk_xlate.csproj" -c Release -r linux-arm64 -p:PublishAot=true -p:InvariantGlobalization=true \
  -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
OUT=$REPO/games/totk/prebuilt/linux-arm64
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
sha256sum "$OUT/totk_xlate" | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
