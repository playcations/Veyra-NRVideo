#!/usr/bin/env bash
set -euo pipefail

ROOT="/opt/veyra-nvdec"
VEYRA_ROOT="/opt/steam-headless/v2/home/Applications/Veyra-2.2.0"
VEYRA_VER="2.2.1"
if [[ -f "${VEYRA_ROOT}/current-version.txt" ]]; then
    VEYRA_VER="$(tr -d '\r\n ' < "${VEYRA_ROOT}/current-version.txt")"
fi
VEYRA_DIR="${VEYRA_ROOT}/versions/${VEYRA_VER}"
SYS32="/opt/steam-headless/v2/home/.local/share/Steam/steamapps/compatdata/3515497427/pfx/drive_c/windows/system32"
NVLIBS="${ROOT}/nvidia-libs/nvidia-libs-v1.0.5/x64"
SHARE="/opt/steam-headless/v2/home/.local/share/veyra"
LAUNCHER="/opt/steam-headless/v2/home/.local/bin/veyra-console"

if [[ ! -f "${ROOT}/build/avcodec-63.dll" ]]; then
    echo "Build artifact missing; running build.sh first..."
    "${ROOT}/scripts/build.sh"
fi

# 1. Stage canonical copies in ~/.local/share/veyra/ so veyra-console auto-heals across updates
install -d -m 0755 -o 3000 -g 3000 "$SHARE"
install -m 0755 -o 3000 -g 3000 "${ROOT}/build/avcodec-63.dll" "${SHARE}/avcodec-63-nvdec.dll"
install -m 0755 -o 3000 -g 3000 "${NVLIBS}/nvcuda.dll" "${SHARE}/nvcuda.dll"
install -m 0755 -o 3000 -g 3000 "${NVLIBS}/nvcuvid.dll" "${SHARE}/nvcuvid.dll"

# 2. Backup original avcodec-63.dll once (never overwrite -orig with our proxy)
if [[ ! -f "${VEYRA_DIR}/avcodec-63-orig.dll" ]]; then
    if grep -q "avcodec-63-orig" "${VEYRA_DIR}/avcodec-63.dll" 2>/dev/null; then
        echo "ERROR: ${VEYRA_DIR}/avcodec-63.dll is already the proxy and avcodec-63-orig.dll is missing!" >&2
        exit 1
    fi
    cp -a "${VEYRA_DIR}/avcodec-63.dll" "${VEYRA_DIR}/avcodec-63-orig.dll"
    chown 3000:3000 "${VEYRA_DIR}/avcodec-63-orig.dll"
fi

# 3. Install NVDEC proxy avcodec-63.dll
install -m 0755 -o 3000 -g 3000 "${SHARE}/avcodec-63-nvdec.dll" "${VEYRA_DIR}/avcodec-63.dll"

# 4. Install nvcuda.dll + nvcuvid.dll into Veyra's Proton prefix (preserving patched nvofapi64.dll)
if [[ -L "${SYS32}/nvcuda.dll" ]]; then
    readlink "${SYS32}/nvcuda.dll" > "${ROOT}/orig-nvcuda-symlink.txt"
    rm -f "${SYS32}/nvcuda.dll"
fi
install -m 0755 -o 3000 -g 3000 "${SHARE}/nvcuda.dll" "${SYS32}/nvcuda.dll"
install -m 0755 -o 3000 -g 3000 "${SHARE}/nvcuvid.dll" "${SYS32}/nvcuvid.dll"

# 5. Ensure veyra-console auto-heals NVDEC files + sets env vars
if ! grep -q "avcodec-63-nvdec.dll" "$LAUNCHER"; then
    if [[ ! -f "${LAUNCHER}.bak-before-nvdec" ]]; then
        cp -a "$LAUNCHER" "${LAUNCHER}.bak-before-nvdec"
        chown 3000:3000 "${LAUNCHER}.bak-before-nvdec"
    fi
    python3 - <<'PY'
p = "/opt/steam-headless/v2/home/.local/bin/veyra-console"
s = open(p).read()
heal_block = '''# Keep our NVDEC proxy + nvcuda/nvcuvid DLLs in place across Steam Proton and Veyra updates.
NVDEC_SHARE="/home/steamheadless/.local/share/veyra"
PFX_SYS32="/home/steamheadless/.local/share/Steam/steamapps/compatdata/3515497427/pfx/drive_c/windows/system32"
for dll in nvcuda.dll nvcuvid.dll; do
    if [[ -f "${NVDEC_SHARE}/${dll}" && -d "${PFX_SYS32}" ]]; then
        if [[ -L "${PFX_SYS32}/${dll}" ]] || ! cmp -s "${NVDEC_SHARE}/${dll}" "${PFX_SYS32}/${dll}" 2>/dev/null; then
            rm -f "${PFX_SYS32}/${dll}" 2>/dev/null || true
            install -m 0755 "${NVDEC_SHARE}/${dll}" "${PFX_SYS32}/${dll}" 2>/dev/null || true
        fi
    fi
done
if [[ -f "${NVDEC_SHARE}/avcodec-63-nvdec.dll" ]]; then
    VEYRA_VER="2.2.1"
    if [[ -f "/home/steamheadless/Applications/Veyra-2.2.0/current-version.txt" ]]; then
        VEYRA_VER="$(tr -d '\\r\\n ' < /home/steamheadless/Applications/Veyra-2.2.0/current-version.txt)"
    fi
    VEYRA_VER_DIR="/home/steamheadless/Applications/Veyra-2.2.0/versions/${VEYRA_VER}"
    if [[ -f "${VEYRA_VER_DIR}/avcodec-63.dll" ]]; then
        if [[ ! -f "${VEYRA_VER_DIR}/avcodec-63-orig.dll" ]] && ! grep -q "avcodec-63-orig" "${VEYRA_VER_DIR}/avcodec-63.dll" 2>/dev/null; then
            cp -a "${VEYRA_VER_DIR}/avcodec-63.dll" "${VEYRA_VER_DIR}/avcodec-63-orig.dll" 2>/dev/null || true
        fi
        if [[ -f "${VEYRA_VER_DIR}/avcodec-63-orig.dll" ]] && ! cmp -s "${NVDEC_SHARE}/avcodec-63-nvdec.dll" "${VEYRA_VER_DIR}/avcodec-63.dll" 2>/dev/null; then
            install -m 0755 "${NVDEC_SHARE}/avcodec-63-nvdec.dll" "${VEYRA_VER_DIR}/avcodec-63.dll" 2>/dev/null || true
        fi
    fi
fi

'''
anchor = "# Reuse the existing window"
assert anchor in s, "anchor not found in veyra-console"
s = s.replace(anchor, heal_block + anchor, 1)
if "VEYRA_NVDEC" not in s:
    env_anchor = '    export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1"\n'
    env_inject = env_anchor + (
        '    export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+${WINEDLLOVERRIDES};}nvcuda=n;nvcuvid=n"\n'
        '    export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"\n'
        '    export VEYRA_NVDEC="${VEYRA_NVDEC:-1}"\n'
    )
    assert env_anchor in s
    s = s.replace(env_anchor, env_inject, 1)
open(p, "w").write(s)
PY
    chown 3000:3000 "$LAUNCHER"
    chmod 0755 "$LAUNCHER"
fi

echo "Installed veyra-nvdec (auto-healing) into ${VEYRA_DIR}, ${SYS32}, and ${SHARE}"
