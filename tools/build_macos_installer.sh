#!/usr/bin/env bash
# Build dist/BE300.pkg by wrapping dist/BE300.app via pkgbuild + productbuild.
# Run tools/build_macos_app.sh first (or this script will do it).
#
# Code signing and notarization are intentionally NOT performed. To add them
# later, install your "Developer ID Application" + "Developer ID Installer"
# certificates and uncomment the productsign / notarytool blocks below.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

VERSION="${BE300_VERSION:-0.1.0}"
APP="dist/BE300.app"
DIST="dist"

if [ ! -d "$APP" ]; then
    echo "[installer] No $APP yet — running build_macos_app.sh first"
    "$ROOT/tools/build_macos_app.sh"
fi

PAYLOAD="$ROOT/build-host/pkg-payload"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/Applications"
cp -R "$APP" "$PAYLOAD/Applications/"

# Component package
pkgbuild \
    --root "$PAYLOAD" \
    --identifier org.casio.be300 \
    --version "$VERSION" \
    --install-location "/" \
    "$ROOT/build-host/BE300-component.pkg"

# Render the distribution.xml with @BE300_VERSION@ substituted
DIST_XML="$ROOT/build-host/distribution.xml"
sed "s/@BE300_VERSION@/${VERSION}/g" \
    "$ROOT/packaging/macos/distribution.xml" > "$DIST_XML"

# Final product
productbuild \
    --distribution "$DIST_XML" \
    --package-path "$ROOT/build-host" \
    "$ROOT/$DIST/BE300.pkg"

# --- Optional signing / notarisation (disabled by default) ----------------
# Set DEVID_INSTALLER to your "Developer ID Installer: ..." name to sign.
# Set APPLE_NOTARY_PROFILE to a `notarytool store-credentials` keychain
# profile to submit for notarisation.
#
# if [ -n "${DEVID_INSTALLER:-}" ]; then
#     productsign --sign "$DEVID_INSTALLER" \
#         "$ROOT/$DIST/BE300.pkg" "$ROOT/$DIST/BE300-signed.pkg"
#     mv "$ROOT/$DIST/BE300-signed.pkg" "$ROOT/$DIST/BE300.pkg"
# fi
# if [ -n "${APPLE_NOTARY_PROFILE:-}" ]; then
#     xcrun notarytool submit "$ROOT/$DIST/BE300.pkg" \
#         --keychain-profile "$APPLE_NOTARY_PROFILE" --wait
#     xcrun stapler staple "$ROOT/$DIST/BE300.pkg"
# fi
# --------------------------------------------------------------------------

echo
echo "Installer written to $ROOT/$DIST/BE300.pkg"
