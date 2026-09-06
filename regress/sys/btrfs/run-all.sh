#!/bin/sh
# Host entry point; Python owns quoting, timeouts, fixtures and VM state.
exec python3 "$(dirname "$0")/run_all.py" "$@"
