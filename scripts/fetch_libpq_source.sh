#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
LIBPQ_SRC_DIR="${THIRD_PARTY_DIR}/libpq-src"

mkdir -p "${THIRD_PARTY_DIR}"

if [ -d "${LIBPQ_SRC_DIR}/.git" ]; then
  echo "libpq source already exists at ${LIBPQ_SRC_DIR}"
  echo "Updating..."
  git -C "${LIBPQ_SRC_DIR}" fetch --depth 1 origin master
  git -C "${LIBPQ_SRC_DIR}" reset --hard FETCH_HEAD
  exit 0
fi

git clone --depth 1 https://github.com/postgres/postgres.git "${LIBPQ_SRC_DIR}"
echo "Fetched postgres source into ${LIBPQ_SRC_DIR}"
