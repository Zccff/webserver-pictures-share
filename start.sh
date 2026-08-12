#!/usr/bin/env bash
set -euo pipefail

export MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
export MYSQL_PORT="${MYSQL_PORT:-3306}"
export MYSQL_USER="${MYSQL_USER:-webuser}"
export MYSQL_PASSWORD="${MYSQL_PASSWORD:-123456}"
export MYSQL_DATABASE="${MYSQL_DATABASE:-my_web_server}"
export REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
export REDIS_PORT="${REDIS_PORT:-6379}"
export REDIS_PASSWORD="${REDIS_PASSWORD:-}"
export SESSION_TTL_SECONDS="${SESSION_TTL_SECONDS:-86400}"

PORT="${1:-9006}"
WEB_ROOT="${2:-./webroot}"
THREAD_COUNT="${3:-}"

if [[ ! -x ./build/my_web_server ]]; then
    echo "build/my_web_server not found. Build first:"
    echo "  cmake -S . -B build && cmake --build build"
    exit 1
fi

echo "Starting MyWebServer"
echo "  URL:      http://127.0.0.1:${PORT}/"
echo "  Web root: ${WEB_ROOT}"
echo "  MySQL:    ${MYSQL_USER}@${MYSQL_HOST}:${MYSQL_PORT}/${MYSQL_DATABASE}"
echo "  Redis:    ${REDIS_HOST}:${REDIS_PORT}"

if [[ -n "${THREAD_COUNT}" ]]; then
    exec ./build/my_web_server "${PORT}" "${WEB_ROOT}" "${THREAD_COUNT}"
else
    exec ./build/my_web_server "${PORT}" "${WEB_ROOT}"
fi
