#!/bin/bash
set -e

MUSL_BASE_URL="https://musl.libc.org"
RELEASES_URL="${MUSL_BASE_URL}/"

echo "Fetching latest musl version..."
LATEST=$(curl -s "${RELEASES_URL}" | grep -o 'musl-[0-9][^"]*\.tar\.gz' | sort -V | tail -1)

if [ -z "${LATEST}" ]; then
	echo "Error: Could not determine latest musl version." >&2
	exit 1
fi

VERSION="${LATEST#musl-}"
VERSION="${VERSION%.tar.gz}"
TARBALL="${LATEST}"
URL="${MUSL_BASE_URL}/releases/${TARBALL}"

echo "Latest musl version: ${VERSION}"
echo "Downloading ${URL}..."
curl -L -O "${URL}"

echo "Extracting ${TARBALL}..."
tar -xzf "${TARBALL}"

echo "Done. Source extracted to: musl-${VERSION}/"
