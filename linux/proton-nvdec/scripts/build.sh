#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_ROOT="${DEPS_ROOT:-/opt/veyra-nvdec}"
VEYRA_DIR="${VEYRA_DIR:-/opt/steam-headless/v2/home/Applications/Veyra-2.2.0/versions/2.2.1}"
FFMPEG_HEADERS="${FFMPEG_HEADERS:-${DEPS_ROOT}/third_party/ffmpeg-n9.0.1}"
NV_CODEC_HEADERS="${NV_CODEC_HEADERS:-${DEPS_ROOT}/third_party/nv-codec-headers/include}"
OUT_DIR="${OUT_DIR:-${ROOT}/build}"

ORIG_DLL="${ORIG_DLL:-${VEYRA_DIR}/avcodec-63-orig.dll}"
if [[ ! -f "$ORIG_DLL" ]]; then
    ORIG_DLL="${VEYRA_DIR}/avcodec-63.dll"
fi

mkdir -p "$OUT_DIR"
python3 "${ROOT}/scripts/gen_def.py" "$ORIG_DLL" "${OUT_DIR}/avcodec-63.def"

CC="${CC:-x86_64-w64-mingw32-gcc-posix}"
CFLAGS=(
    -O2 -Wall -Wextra -Wno-deprecated-declarations -Wno-cast-function-type -Wno-unused-parameter
    -I"${FFMPEG_HEADERS}"
    -I"${NV_CODEC_HEADERS}"
)

echo "Building ${OUT_DIR}/avcodec-63.dll ..."
"$CC" "${CFLAGS[@]}" -shared \
    -o "${OUT_DIR}/avcodec-63.dll" \
    "${ROOT}/src/avcodec_nvdec_proxy.c" \
    "${OUT_DIR}/avcodec-63.def" \
    -Wl,--enable-stdcall-fixup

echo "Building ${OUT_DIR}/test_nvdec.exe ..."
"$CC" "${CFLAGS[@]}" \
    -o "${OUT_DIR}/test_nvdec.exe" \
    "${ROOT}/src/test_nvdec.c"

ls -lh "${OUT_DIR}/avcodec-63.dll" "${OUT_DIR}/test_nvdec.exe"
