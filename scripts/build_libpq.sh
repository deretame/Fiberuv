#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${ROOT_DIR}/third_party/libpq-src"
BUILD_DIR="${ROOT_DIR}/third_party/libpq-build"
INSTALL_DIR="${ROOT_DIR}/third_party/libpq-install"

if [ ! -d "${SRC_DIR}" ]; then
  echo "Missing libpq source at ${SRC_DIR}" >&2
  echo "Run ./scripts/fetch_libpq_source.sh first." >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "config.status" ]; then
  BISON_BIN="${BISON:-$(command -v bison || true)}"
  FLEX_BIN="${FLEX:-$(command -v flex || true)}"
  if [ -z "${BISON_BIN}" ]; then
    BISON_BIN="/usr/bin/true"
    echo "bison not found, using ${BISON_BIN} for configure-time checks."
  fi
  if [ -z "${FLEX_BIN}" ]; then
    FLEX_BIN="/usr/bin/true"
    echo "flex not found, using ${FLEX_BIN} for configure-time checks."
  fi

  BISON="${BISON_BIN}" FLEX="${FLEX_BIN}" "${SRC_DIR}/configure" \
    --prefix="${INSTALL_DIR}" \
    --without-readline \
    --without-zlib \
    --without-icu
fi

make -j"$(nproc)" -C src/interfaces/libpq
make -C src/interfaces/libpq install

mkdir -p "${INSTALL_DIR}/include"
cp -f "${SRC_DIR}/src/include/postgres_ext.h" "${INSTALL_DIR}/include/postgres_ext.h"

echo "libpq installed to ${INSTALL_DIR}"
