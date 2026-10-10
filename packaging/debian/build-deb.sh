#!/bin/bash
set -euo pipefail

# Determine script root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERSION="${1:-0.2.0}"
PKG_NAME="minisforum-platform-dkms"
MOD_NAME="minisforum_platform"
BUILD_DIR="$(mktemp -d /tmp/deb-build-XXXXXX)"
DEB_FILE="${PKG_NAME}_${VERSION}_all.deb"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

echo "Building Debian/Ubuntu DKMS package: ${DEB_FILE}..."

# Setup directory structure
mkdir -p "${BUILD_DIR}/DEBIAN"
mkdir -p "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/src"
mkdir -p "${BUILD_DIR}/usr/share/doc/${PKG_NAME}"

# Populate DEBIAN control and scripts
sed "s/Version: .*/Version: ${VERSION}/" "${SCRIPT_DIR}/control" > "${BUILD_DIR}/DEBIAN/control"
sed "s/PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"${VERSION}\"/" "${SCRIPT_DIR}/postinst" > "${BUILD_DIR}/DEBIAN/postinst"
sed "s/PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"${VERSION}\"/" "${SCRIPT_DIR}/prerm" > "${BUILD_DIR}/DEBIAN/prerm"
chmod 0755 "${BUILD_DIR}/DEBIAN/postinst" "${BUILD_DIR}/DEBIAN/prerm"

# Populate DKMS source files
cp "${REPO_ROOT}/Makefile" "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/"
cp "${REPO_ROOT}/dkms.conf" "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/"
cp "${REPO_ROOT}/src/"*.[ch] "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/src/"

sed -i "s/PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"${VERSION}\"/" "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/dkms.conf"
sed -i "s/VERSION := .*/VERSION := ${VERSION}/" "${BUILD_DIR}/usr/src/${MOD_NAME}-${VERSION}/Makefile"

# Documentation
cp "${REPO_ROOT}/README.md" "${BUILD_DIR}/usr/share/doc/${PKG_NAME}/"
cp "${REPO_ROOT}/LICENSE" "${BUILD_DIR}/usr/share/doc/${PKG_NAME}/copyright"

# Build debian package
dpkg-deb --build --root-owner-group "${BUILD_DIR}" "${REPO_ROOT}/${DEB_FILE}"

echo "Successfully built: ${REPO_ROOT}/${DEB_FILE}"
