#!/usr/bin/env bash
# Re-vendors the Strata web kit's loading shell (Strata's web/kit/strata-loader.js)
# at one commit:
#   web/strata-kit/update.sh <full commit sha>
# then rebuild, run the browser smoke test, and commit the loader, PIN and SHA256SUMS.
# Only the loader is vendored: the page is a classic Emscripten build and
# web/strata-boot.js bridges it to the loader. Once a Strata release carries the
# kit (its wasm tarball has kit/), the loader can come from the tarball instead.
set -euo pipefail
commit="${1:?usage: update.sh <full strata commit sha>}"
case "$commit" in *[!0-9a-f]* | "") echo "update.sh: give a full hex commit sha" >&2; exit 1 ;; esac
[ "${#commit}" -eq 40 ] || { echo "update.sh: give the full 40-character sha" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
base="https://raw.githubusercontent.com/Preponderous-Software/strata/$commit"
curl -fsSL -o "$here/strata-loader.js" "$base/web/kit/strata-loader.js"
curl -fsSL -o "$here/LICENSE" "$base/LICENSE"
echo "$commit" > "$here/PIN"
(cd "$here" && sha256sum strata-loader.js LICENSE > SHA256SUMS)
echo "vendored the Strata loading shell at $commit"
