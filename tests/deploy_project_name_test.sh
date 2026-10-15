#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'case "$tmp_dir" in /tmp/tmp.*) rm -rf -- "$tmp_dir";; esac' EXIT

cp "$project_root/deploy/offline/deploy.sh" "$tmp_dir/deploy.sh"
touch "$tmp_dir/mywebserver-app-opt-v5-linux-amd64.tar"
printf 'COMPOSE_PROJECT_NAME=mywebserver-custom\nPUBLIC_PORT=8080\n' > "$tmp_dir/.env"
export DEPLOY_TEST_LOG="$tmp_dir/docker-commands.log"

docker() {
    printf '%s\n' "$*" >> "$DEPLOY_TEST_LOG"
    return 0
}
curl() { return 0; }
export -f docker curl

env -u COMPOSE_PROJECT_NAME bash "$tmp_dir/deploy.sh" > "$tmp_dir/output.log"

if ! grep -q '^compose -p mywebserver-custom --env-file .env' \
    "$DEPLOY_TEST_LOG"; then
    echo 'deploy.sh ignored COMPOSE_PROJECT_NAME from .env' >&2
    cat "$DEPLOY_TEST_LOG" >&2
    exit 1
fi

echo 'deploy project name test passed'
