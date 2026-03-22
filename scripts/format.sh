#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if command -v clang-format >/dev/null 2>&1; then
  CLANG_FORMAT_BIN="$(command -v clang-format)"
elif [ -x "${ROOT_DIR}/.tools/clangfmt19/usr/lib/llvm-19/bin/clang-format" ]; then
  CLANG_FORMAT_BIN="${ROOT_DIR}/.tools/clangfmt19/usr/lib/llvm-19/bin/clang-format"
  export LD_LIBRARY_PATH="${ROOT_DIR}/.tools/clangfmt19/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
else
  echo "clang-format not found. Please install it or place local binary under .tools/clangfmt19." >&2
  exit 1
fi

mapfile -t FILES < <(
  rg --files "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/examples" \
    -g "*.hpp" -g "*.h" -g "*.cpp" -g "*.cc" -g "*.cxx"
)

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "No C/C++ source files found."
  exit 0
fi

"${CLANG_FORMAT_BIN}" -i "${FILES[@]}"
echo "Formatted ${#FILES[@]} files."
