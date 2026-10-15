#!/usr/bin/env bash
set -euo pipefail

mkdir -p /app/webroot/images /app/webroot/images/thumbnails /app/webroot/images/.upload-tmp

if [ -d /app/webroot-seed/images ] && [ -z "$(ls -A /app/webroot/images 2>/dev/null)" ]; then
    cp -a /app/webroot-seed/images/. /app/webroot/images/
fi

exec "$@"
