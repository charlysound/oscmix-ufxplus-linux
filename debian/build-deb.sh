#!/bin/sh
# Build the UFX+ GTK4 application as a Debian package.
# Run from any directory: ./debian/build-deb.sh VERSION

set -eu

if [ "$#" -ne 1 ]; then
	printf 'Usage: %s VERSION\n' "$0" >&2
	printf 'Example: %s 0.1.0~alpha2-1\n' "$0" >&2
	exit 2
fi

VERSION=$1
case "$VERSION" in
	[0-9]*)
		;;
	*)
		printf 'Error: Debian package versions must start with a digit.\n' >&2
		exit 2
		;;
esac
case "$VERSION" in
	*[!0-9A-Za-z.+:~-]*)
		printf 'Error: unsupported character in package version: %s\n' "$VERSION" >&2
		exit 2
		;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
ARCH=$(dpkg --print-architecture)
PACKAGE=oscmix-ufxplus
APP_DIR=/usr/lib/oscmix-ufxplus
OUTPUT_DIR="${REPO_ROOT}/dist"
OUTPUT="${OUTPUT_DIR}/${PACKAGE}_${VERSION}_${ARCH}.deb"
BUILD_ROOT=$(mktemp -d)
PACKAGE_ROOT="${BUILD_ROOT}/${PACKAGE}"
DOC_DIR="${PACKAGE_ROOT}/usr/share/doc/${PACKAGE}"

cleanup()
{
	rm -rf -- "$BUILD_ROOT"
}
trap cleanup EXIT HUP INT TERM

cd "$REPO_ROOT"

printf 'Building oscmix GTK4 and its UFX+ backend...\n'
make -j"${JOBS:-2}" oscmix alsaseqio gtk4
make -C gtk4 check

install -d \
	"${PACKAGE_ROOT}${APP_DIR}/bin" \
	"${PACKAGE_ROOT}/usr/bin" \
	"${PACKAGE_ROOT}/usr/share/applications" \
	"${PACKAGE_ROOT}/usr/share/icons/hicolor/scalable/apps" \
	"$DOC_DIR" \
	"${PACKAGE_ROOT}/DEBIAN" \
	"$OUTPUT_DIR"

install -m 755 oscmix "${PACKAGE_ROOT}${APP_DIR}/bin/oscmix"
install -m 755 alsaseqio "${PACKAGE_ROOT}${APP_DIR}/bin/alsaseqio"
install -m 755 gtk4/oscmix-gtk4 "${PACKAGE_ROOT}${APP_DIR}/bin/oscmix-gtk4"
install -m 755 gtk4/AppRun "${PACKAGE_ROOT}${APP_DIR}/AppRun"
strip --strip-unneeded \
	"${PACKAGE_ROOT}${APP_DIR}/bin/oscmix" \
	"${PACKAGE_ROOT}${APP_DIR}/bin/alsaseqio" \
	"${PACKAGE_ROOT}${APP_DIR}/bin/oscmix-gtk4"
ln -s "${APP_DIR}/AppRun" "${PACKAGE_ROOT}/usr/bin/oscmix-ufxplus"

install -m 644 gtk/oscmix.svg \
	"${PACKAGE_ROOT}/usr/share/icons/hicolor/scalable/apps/oscmix-ufxplus.svg"
install -m 644 debian/copyright "${DOC_DIR}/copyright"
install -m 644 README.md "${DOC_DIR}/README.md"

cat >"${PACKAGE_ROOT}/usr/share/applications/oscmix-ufxplus.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=oscmix GTK4 – UFX+
GenericName=Fireface Mixer
Comment=Lightweight mixer for RME Fireface UFX+ in Class Compliant mode
Exec=oscmix-ufxplus
Icon=oscmix-ufxplus
Terminal=false
Categories=AudioVideo;Audio;Mixer;
Keywords=RME;Fireface;UFX;TotalMix;OSC;Mixer;
StartupNotify=true
EOF
chmod 644 "${PACKAGE_ROOT}/usr/share/applications/oscmix-ufxplus.desktop"

{
	printf '%s (%s) unstable; urgency=medium\n\n' "$PACKAGE" "$VERSION"
	git log --format='  * %s' -10
	printf '\n -- charlysound <151044893+charlysound@users.noreply.github.com>  %s\n' \
		"$(date -R)"
} | gzip -9 -n >"${DOC_DIR}/changelog.Debian.gz"
chmod 644 "${DOC_DIR}/changelog.Debian.gz"

INSTALLED_SIZE=$(du -sk "$PACKAGE_ROOT" | cut -f1)
cat >"${PACKAGE_ROOT}/DEBIAN/control" <<EOF
Package: ${PACKAGE}
Version: ${VERSION}
Section: sound
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: bash, alsa-utils, iproute2, libasound2, libavahi-client3, libgtk-4-1 (>= 4.6), zenity
Maintainer: charlysound <151044893+charlysound@users.noreply.github.com>
Homepage: https://github.com/charlysound/oscmix-ufxplus-linux
Description: GTK4 mixer for RME Fireface UFX+ on Linux
 Unofficial lightweight mixer and OSC/MIDI control frontend for the RME
 Fireface UFX+ connected over USB in Class Compliant mode.
 .
 This package includes the GTK4 frontend, oscmix engine, ALSA sequencer bridge
 and a desktop launcher with automatic UFX+ discovery and reconnection.
EOF
chmod 644 "${PACKAGE_ROOT}/DEBIAN/control"

dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$OUTPUT"
printf 'Built %s\n' "$OUTPUT"
