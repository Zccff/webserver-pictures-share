#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker command not found"
    exit 1
fi

if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE=(docker-compose)
else
    echo "Docker Compose was not found"
    exit 1
fi

if [[ ! -f .env ]]; then
    cp .env.example .env
    echo "Created .env. Replace CHANGE_ME passwords, then run ./deploy.sh again."
    exit 1
fi

if grep -q "CHANGE_ME" .env; then
    echo ".env still contains CHANGE_ME passwords; deployment stopped."
    exit 1
fi

IMAGE_ARCHIVE="mywebserver-app-opt-v5-linux-amd64.tar"
PROJECT_NAME="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' .env | tail -n 1 | tr -d '\r')"
PROJECT_NAME="${PROJECT_NAME:-mywebserver-v5}"

if [[ ! -f "$IMAGE_ARCHIVE" ]]; then
    echo "Missing $IMAGE_ARCHIVE"
    exit 1
fi

docker load -i "$IMAGE_ARCHIVE"
docker image inspect mywebserver-app:opt-v5 >/dev/null
"${COMPOSE[@]}" -p "$PROJECT_NAME" --env-file .env -f docker-compose.yml config --quiet
"${COMPOSE[@]}" -p "$PROJECT_NAME" --env-file .env -f docker-compose.yml up -d --no-build
"${COMPOSE[@]}" -p "$PROJECT_NAME" --env-file .env -f docker-compose.yml ps

PUBLIC_PORT="$(sed -n 's/^PUBLIC_PORT=//p' .env | tail -n 1)"
PUBLIC_PORT="${PUBLIC_PORT:-8080}"

for _ in $(seq 1 30); do
    if command -v curl >/dev/null 2>&1 &&
        curl -fsS "http://127.0.0.1:${PUBLIC_PORT}/metrics" >/dev/null; then
        echo "Deployment ready: http://SERVER_IP:${PUBLIC_PORT}/"
        exit 0
    fi

    sleep 2
done

echo "Containers started, but the health probe did not pass within 60 seconds."
"${COMPOSE[@]}" -p "$PROJECT_NAME" --env-file .env -f docker-compose.yml logs --tail=100 app nginx
exit 1
