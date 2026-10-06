#!/bin/sh
# Builds the wasm core and packages web/ as mdcore-web-<version>.zip in dist/.
set -eu
cd "$(dirname "$0")"
VERSION=$(sed -n 's/^#define MD_VERSION "\(.*\)"/\1/p' src/mdcore.h)
./build.sh
mkdir -p dist
rm -f "dist/mdcore-web-$VERSION.zip"
cp LICENSE web/LICENSE
(cd web && zip -q -9 "../dist/mdcore-web-$VERSION.zip" index.html player.js mdcore.js audio-worklet.js mdcore.wasm LICENSE)
rm web/LICENSE
cp web/mdcore.wasm "dist/mdcore-$VERSION.wasm"
(cd dist && sha256sum "mdcore-web-$VERSION.zip" "mdcore-$VERSION.wasm" > SHA256SUMS)
ls -l dist
