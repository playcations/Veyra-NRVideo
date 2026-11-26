#!/usr/bin/env bash
set -euo pipefail

ROOT="/opt/veyra-nvdec"
VEYRA_DIR="/opt/steam-headless/v2/home/Applications/Veyra-2.2.0/versions/2.2.1"
SYS32="/opt/steam-headless/v2/home/.local/share/Steam/steamapps/compatdata/3515497427/pfx/drive_c/windows/system32"
LAUNCHER="/opt/steam-headless/v2/home/.local/bin/veyra-console"

if [[ -f "${VEYRA_DIR}/avcodec-63-orig.dll" ]]; then
    mv -f "${VEYRA_DIR}/avcodec-63-orig.dll" "${VEYRA_DIR}/avcodec-63.dll"
    chown 3000:3000 "${VEYRA_DIR}/avcodec-63.dll"
fi

if [[ -f "${ROOT}/orig-nvcuda-symlink.txt" ]]; then
    rm -f "${SYS32}/nvcuda.dll" "${SYS32}/nvcuvid.dll"
    ln -sf "$(cat "${ROOT}/orig-nvcuda-symlink.txt")" "${SYS32}/nvcuda.dll"
    chown -h 3000:3000 "${SYS32}/nvcuda.dll"
fi

if [[ -f "${LAUNCHER}.bak-before-nvdec" ]]; then
    mv -f "${LAUNCHER}.bak-before-nvdec" "$LAUNCHER"
    chown 3000:3000 "$LAUNCHER"
    chmod 0755 "$LAUNCHER"
fi

echo "Uninstalled veyra-nvdec and restored stock Veyra avcodec-63.dll"
