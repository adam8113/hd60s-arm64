#!/bin/bash
# Live USB bench for soft reset / colour path. Run in a normal Terminal: IOKit
# user clients return e00002be from sandboxed shells.
set -e
cd "$(dirname "$0")/.."
OUT="${1:-/tmp/hd60s-bench.txt}"
exec > >(tee "$OUT") 2>&1

echo "=== host $(date) ==="
echo "=== PROBE ==="
./start probe || true
echo "=== RESET ==="
./start reset
echo "=== COLOUR (may defer if no InfoFrame yet) ==="
./start colour || true
echo "=== MEASURE ==="
./start measure || true
echo "=== RESTART 1 ==="
./start restart
echo "=== RESTART 2 (warm recovery, no cable yank) ==="
./start restart
echo "=== COLOUR after restart ==="
./start colour || true
echo "=== LINK ==="
ioreg -p IOUSB -l -w0 | awk '/Game Capture HD60 S@/{p=1} p&&/UsbLinkSpeed|Device Speed|locationID/{print} p&&/^\s+\|/{if(/Game Capture/==0 && /Hub|XHCI|AppleUSB/) exit}'
echo DONE
