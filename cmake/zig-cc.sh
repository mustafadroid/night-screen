#!/bin/sh
# C wrapper: zig cc targeting 64-bit Windows.
# Set ZIG_DIR to the folder containing zig-x86_64-linux-0.15.1/ (default /tmp/opencode).
ZIG_ROOT="${ZIG_DIR:-/tmp/opencode}/zig-x86_64-linux-0.15.1/zig"
exec "$ZIG_ROOT" cc -target x86_64-windows-gnu "$@"
