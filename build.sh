#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
OUT="${1:-./usage-metrics.so}"
python3 gen_ui_header.py
gcc -shared -fPIC -O2 -Wall -Wextra -Werror -pthread plugin.c -o "$OUT"
ls -lh "$OUT"
