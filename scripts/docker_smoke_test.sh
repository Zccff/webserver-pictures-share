#!/usr/bin/env bash
set -euo pipefail

base_url="${SMOKE_BASE_URL:-http://127.0.0.1:18080}"
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="${SMOKE_IMAGE_FIXTURE:-$project_root/webroot/images/city-street.png}"
run_suffix="${SMOKE_RUN_SUFFIX:-$(date +%s)_$$}"
user_one="smoke_one_$run_suffix"
user_two="smoke_two_$run_suffix"
idempotency_one="smoke-upload-$run_suffix-0001"
idempotency_two="smoke-upload-$run_suffix-0002"
tmp_dir="$(mktemp -d)"
cookie_one="$tmp_dir/cookie-one.txt"
cookie_two="$tmp_dir/cookie-two.txt"
body="$tmp_dir/body.json"

cleanup() {
    case "$tmp_dir" in
        /tmp/tmp.*) rm -rf -- "$tmp_dir" ;;
    esac
}
trap cleanup EXIT

require_status() {
    local actual="$1"
    local expected="$2"
    local operation="$3"
    if [[ "$actual" != "$expected" ]]; then
        echo "$operation: expected HTTP $expected, received $actual" >&2
        if [[ -f "$body" ]]; then
            tr -d '\n' < "$body" >&2
            echo >&2
        fi
        exit 1
    fi
}

request_status() {
    curl -sS -o "$body" -w "%{http_code}" "$@"
}

[[ -f "$fixture" ]] || {
    echo "Smoke image fixture not found: $fixture" >&2
    exit 1
}

home_status="$(curl -sS -o /dev/null -w "%{http_code}" "$base_url/")"
metrics_status="$(curl -sS -o /dev/null -w "%{http_code}" "$base_url/metrics")"
require_status "$home_status" 200 "homepage"
require_status "$metrics_status" 200 "metrics"

db_status="$(curl -fsS "$base_url/api/db-status")"
redis_status="$(curl -fsS "$base_url/api/session-status")"
grep -q '"available": true' <<<"$db_status"
grep -q '"available": true' <<<"$redis_status"

unauthenticated="$(request_status -X POST \
    -H "Content-Type: image/png" \
    -H "X-File-Name: smoke.png" \
    -H "Idempotency-Key: smoke-unauth-001" \
    --data-binary "@$fixture" \
    "$base_url/api/upload-image")"
require_status "$unauthenticated" 401 "unauthenticated upload"

invalid_registration="$(request_status -X POST \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "username=x&password=1&display_name=x&email=bad" \
    "$base_url/api/register")"
require_status "$invalid_registration" 400 "invalid registration"

register_one="$(request_status -X POST \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "username=$user_one&password=SmokePass123&display_name=Smoke%20One&email=$user_one%40example.com" \
    "$base_url/api/register")"
require_status "$register_one" 200 "first registration"

login_one="$(curl -sS -c "$cookie_one" -o "$body" -w "%{http_code}" \
    -X POST \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "username=$user_one&password=SmokePass123" \
    "$base_url/api/login")"
require_status "$login_one" 200 "first login"

curl -fsS -b "$cookie_one" "$base_url/api/me" | grep -q "$user_one"

register_two="$(request_status -X POST \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "username=$user_two&password=SmokePass456&display_name=Smoke%20Two&email=$user_two%40example.com" \
    "$base_url/api/register")"
require_status "$register_two" 200 "second registration"

login_two="$(curl -sS -c "$cookie_two" -o "$body" -w "%{http_code}" \
    -X POST \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "username=$user_two&password=SmokePass456" \
    "$base_url/api/login")"
require_status "$login_two" 200 "second login"

upload="$(curl -sS -b "$cookie_one" -o "$body" -w "%{http_code}" \
    -X POST \
    -H "Accept: application/json" \
    -H "Content-Type: image/png" \
    -H "X-File-Name: smoke.png" \
    -H "Idempotency-Key: $idempotency_one" \
    --data-binary "@$fixture" \
    "$base_url/api/upload-image")"
require_status "$upload" 200 "image upload"
image_name="$(sed -n 's/.*"name": "\([^"]*\)".*/\1/p' "$body")"
[[ -n "$image_name" ]] || {
    echo "Upload response did not contain an image name" >&2
    exit 1
}

replay="$(curl -sS -b "$cookie_one" -o "$body" -w "%{http_code}" \
    -X POST \
    -H "Content-Type: image/png" \
    -H "X-File-Name: smoke.png" \
    -H "Idempotency-Key: $idempotency_one" \
    --data-binary "@$fixture" \
    "$base_url/api/upload-image")"
require_status "$replay" 200 "idempotent replay"

duplicate="$(curl -sS -b "$cookie_one" -o "$body" -w "%{http_code}" \
    -X POST \
    -H "Content-Type: image/png" \
    -H "X-File-Name: smoke-copy.png" \
    -H "Idempotency-Key: $idempotency_two" \
    --data-binary "@$fixture" \
    "$base_url/api/upload-image")"
require_status "$duplicate" 409 "duplicate upload"

thumbnail_ready=false
for _ in $(seq 1 30); do
    status="$(request_status -b "$cookie_one" \
        "$base_url/api/image-status?name=$image_name")"
    require_status "$status" 200 "thumbnail status"
    if grep -q 'READY' "$body"; then
        thumbnail_ready=true
        break
    fi
    sleep 0.2
done
[[ "$thumbnail_ready" == true ]] || {
    echo "Thumbnail did not become READY" >&2
    exit 1
}

image_status="$(curl -sS -b "$cookie_one" -o /dev/null -w "%{http_code}" \
    "$base_url/images/$image_name")"
thumbnail_status="$(curl -sS -b "$cookie_one" -o /dev/null -w "%{http_code}" \
    "$base_url/thumbnails/${image_name%.*}.webp")"
download_status="$(curl -sS -b "$cookie_one" -o /dev/null -w "%{http_code}" \
    "$base_url/api/download?name=$image_name")"
require_status "$image_status" 200 "protected image"
require_status "$thumbnail_status" 200 "protected thumbnail"
require_status "$download_status" 200 "image download"

ranking_ready=false
for _ in $(seq 1 20); do
    views="$(curl -fsS -b "$cookie_one" \
        "$base_url/api/rankings?type=views&limit=5")"
    downloads="$(curl -fsS -b "$cookie_one" \
        "$base_url/api/rankings?type=downloads&limit=5")"
    if grep -q "$image_name" <<<"$views" &&
        grep -q "$image_name" <<<"$downloads"; then
        ranking_ready=true
        break
    fi
    sleep 0.1
done
[[ "$ranking_ready" == true ]] || {
    echo "Rankings did not include the viewed and downloaded image" >&2
    exit 1
}

foreign_delete="$(request_status -b "$cookie_two" -X DELETE \
    "$base_url/api/images?name=$image_name")"
require_status "$foreign_delete" 403 "foreign image deletion"

owner_delete="$(request_status -b "$cookie_one" -X DELETE \
    "$base_url/api/images?name=$image_name")"
require_status "$owner_delete" 200 "owner image deletion"

deleted_image="$(curl -sS -b "$cookie_one" -o /dev/null -w "%{http_code}" \
    "$base_url/images/$image_name")"
require_status "$deleted_image" 404 "deleted image access"

metrics="$(curl -fsS "$base_url/metrics")"
grep -Eq '"duplicate_uploads": [1-9][0-9]*' <<<"$metrics"

echo "Docker smoke test passed"
echo "  image: $image_name"
echo "  homepage/metrics: 200/200"
echo "  upload/replay/duplicate: 200/200/409"
echo "  protected image/thumbnail/download: 200/200/200"
echo "  foreign delete/owner delete/post-delete: 403/200/404"
