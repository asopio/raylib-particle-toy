#!/usr/bin/env bash
# Serve the web build from the repo root.
# Usage: ./serve.sh [port]   (default 8000)
set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-8000}"

if [[ ! -f bubble_chamber.html ]]; then
    echo "bubble_chamber.html not found - run 'pixi run make web' first." >&2
    exit 1
fi

echo "Serving http://localhost:${PORT}/bubble_chamber.html"
exec pixi run python -m http.server "$PORT"
