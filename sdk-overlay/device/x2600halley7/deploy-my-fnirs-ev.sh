#!/bin/bash
# Backward-compatible wrapper — use deploy-my-fnirs.sh
exec "$(dirname "$0")/deploy-my-fnirs.sh" "$@"
