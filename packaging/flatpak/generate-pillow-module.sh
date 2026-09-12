#!/bin/sh
# Generate packaging/flatpak/python3-pillow.yml, the one module
# io.lairware.Ultima3.yml expects to exist but does not carry itself.
#
# Why it is generated rather than committed: Pillow is needed only at build
# time, by scripts/export-portable-assets.py, to decode the game's PNG/GIF/JPEG
# art into the raw RGBA bundle the runtime reads. A Flatpak source has to pin a
# URL and sha256 for every wheel in the tree, and hand-written pins for a
# package this build does not otherwise constrain would rot silently. Flathub
# solves this with flatpak-pip-generator, so this does too.
#
# Requires network access and flatpak-pip-generator, which ships with
# flatpak-builder-tools:
#
#   git clone https://github.com/flatpak/flatpak-builder-tools
#   PIP_GENERATOR=flatpak-builder-tools/pip/flatpak-pip-generator \
#       sh packaging/flatpak/generate-pillow-module.sh
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
generator="${PIP_GENERATOR:-$(command -v flatpak-pip-generator 2>/dev/null || true)}"

if [ -z "$generator" ]; then
    cat >&2 <<'EOF'
error: flatpak-pip-generator not found.

Get it from flatpak-builder-tools and point PIP_GENERATOR at it:

    git clone https://github.com/flatpak/flatpak-builder-tools
    PIP_GENERATOR=flatpak-builder-tools/pip/flatpak-pip-generator \
        sh packaging/flatpak/generate-pillow-module.sh
EOF
    exit 1
fi

echo "==> Generating the python3-pillow module with $generator"
cd "$here"
"$generator" --yaml --output python3-pillow pillow

if [ ! -f python3-pillow.yml ]; then
    echo "error: the generator did not produce python3-pillow.yml" >&2
    exit 1
fi

echo "Wrote $here/python3-pillow.yml"
echo "io.lairware.Ultima3.yml can now be built with flatpak-builder."
