#!/usr/bin/env python3
"""Generate avcodec-63.def forwarding all untouched exports to avcodec-63-orig.dll."""
import subprocess
import sys

ORIG_DLL_PATH = sys.argv[1]
OUT_DEF_PATH = sys.argv[2]

INTERCEPTED = {
    "avcodec_open2",
    "avcodec_send_packet",
    "avcodec_receive_frame",
    "avcodec_flush_buffers",
    "avcodec_free_context",
}

out = subprocess.check_output(["objdump", "-p", ORIG_DLL_PATH]).decode("utf-8", "ignore")
in_exp = False
exports = []
for line in out.splitlines():
    if "[Ordinal/Name Pointer] Table" in line:
        in_exp = True
        continue
    if in_exp:
        s = line.strip()
        if not s:
            break
        if s.startswith("["):
            name = s.split()[-1].strip()
            exports.append(name)

assert len(exports) > 100, f"unexpected export count: {len(exports)}"
missing = INTERCEPTED - set(exports)
assert not missing, f"missing intercepted exports: {missing}"

with open(OUT_DEF_PATH, "w") as f:
    f.write("LIBRARY avcodec-63.dll\n")
    f.write("EXPORTS\n")
    for name in exports:
        if name in INTERCEPTED:
            f.write(f"    {name}\n")
        else:
            f.write(f"    {name}=avcodec-63-orig.{name}\n")

print(f"Wrote {len(exports)} exports ({len(INTERCEPTED)} intercepted, {len(exports) - len(INTERCEPTED)} forwarded) to {OUT_DEF_PATH}")
