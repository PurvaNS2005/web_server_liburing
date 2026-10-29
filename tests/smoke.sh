#!/usr/bin/env bash
# Compatibility entry point; the regression suite uses isolated temporary roots.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/tests/integration.py" --server "${SERVER_BIN:-$ROOT/build/webserver_liburing}"
