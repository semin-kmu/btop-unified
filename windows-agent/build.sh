#!/usr/bin/env bash
# Build bin/btop-agent.exe with MSVC from WSL (WSL -> cmd.exe -> build.bat -> vcvars64 -> cl).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build/tmp" "$here/bin"
bat="$(wslpath -w "$here/build.bat")"
cd /mnt/c  # avoid cmd.exe's UNC-cwd warning
/mnt/c/Windows/System32/cmd.exe /d /c "$bat" | iconv -f cp949 -t utf-8 -c
exit "${PIPESTATUS[0]}"
