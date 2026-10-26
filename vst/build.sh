#!/usr/bin/env bash
# Build Tuner as an MPC OS VST2 insert effect with mpc-vst-plugins' generic port builder (tools/build_port.sh):
# vst/build/tuner.so (Gen1, armv7), vst/build/aarch64/tuner.so (Gen2), the skin folder and the pluginlist entry.
# Needs Docker (QEMU for the arm images) and an mpc-vst-plugins checkout (MPC_VST, default: next to this repo).
# Usage: vst/build.sh [armv7|aarch64]   (default: both)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:-$here/../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/build_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
exec "$MPC_VST/tools/build_port.sh" "$here/vst/vst.json" "$@"
