#!/usr/bin/env bash
# Starts this game on Linux / macOS (built with tools/ps2port; see port.toml). Twin of Play.cmd.
here="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$here/../../tools/ps2port/ps2port.py" play --port "$here" "$@"
