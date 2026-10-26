#!/usr/bin/env bash
# Offline tests (x86, ASan/UBSan) before anything goes to a device:
#   1. tests/pitch_test.c: the pitch detector against known tones, the hold after silence, pass-through and mute
#   2. mpc-vst-plugins tools/test_port.sh: the generic wrapper's host test (instances, params, stepping)
# Needs an mpc-vst-plugins checkout (MPC_VST, default: next to this repo) and Docker for step 2.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export MPC_VST="${MPC_VST:-$here/../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/test_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
cd "$here"
mkdir -p build
cc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Isrc -I"$MPC_VST/wrapper" \
    -o build/pitch_test tests/pitch_test.c -lm
build/pitch_test
"$MPC_VST/tools/test_port.sh" "$here/vst/vst.json"
