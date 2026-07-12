#!/bin/bash
set -e

# Pinned (not "latest", unlike get-musl.sh): the app/lwip_port glue
# (lwipopts.h, arch/cc.h, arch/sys_arch.h) and app/net_glue.c/
# net_shim.c are written against this exact release's file layout and
# config option set. Bump deliberately, not automatically.
VERSION="2.2.0"
URL="https://download.savannah.nongnu.org/releases/lwip/lwip-${VERSION}.zip"
ZIPFILE="lwip-${VERSION}.zip"

echo "Downloading ${URL}..."
curl -L -o "${ZIPFILE}" "${URL}"

echo "Extracting ${ZIPFILE}..."
unzip -q -o "${ZIPFILE}"

echo "Done. Source extracted to: lwip-${VERSION}/"
