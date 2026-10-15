# Redis Platform Design

## Goal

Turn the existing hiredis/libuv session-only pool into one shared asynchronous Redis transport and use it for sessions, image state cache, image list cache, per-user upload limits, and upload idempotency without blocking SubReactors or worker threads on Redis I/O.

## Scope

This phase implements the Redis data plane only. MySQL remains the source of truth for users, image ownership, image count, and duplicate-image uniqueness. Image streaming, libvips thumbnail generation, durable image processing states, and EPOLLET are separate follow-up phases.

## Architecture

- `AsyncRedisClient` owns the libuv loop thread, the hiredis connection pool, bounded command queue, per-connection pipeline limits, reconnect handling, and command deadlines.
- `AsyncSessionManager` becomes a session-domain adapter over `AsyncRedisClient` and retains its short-TTL process-local session cache.
- `ImageRedisService` owns Redis key construction and typed operations for image list/status caching, upload rate limiting, concurrent-upload permits, and idempotency.
- `HttpResponse` composes these asynchronous operations before scheduling blocking filesystem/MySQL image work back to the worker pool.
- All Redis callbacks may run on the Redis loop thread. They may construct small cached responses or enqueue worker continuations, but must not perform MySQL queries, filesystem scans, image decoding, or blocking waits.

## Keyspace

The configurable prefix is `REDIS_KEY_PREFIX`, defaulting to `myweb:v5:`.

- Session: `myweb:v5:session:<token>`
- Global shared-gallery list: `myweb:v5:image:list:all`
- Image status: `myweb:v5:image:status:<saved-name>`
- Per-minute upload rate: `myweb:v5:upload:rate:<username>:<unix-minute>`
- Concurrent upload count: `myweb:v5:upload:active:<username>`
- Upload idempotency: `myweb:v5:upload:idem:<username>:<idempotency-key>`

Usernames and saved image names are already application-validated. `Idempotency-Key` is accepted only when it is 8-128 characters and contains ASCII letters, digits, `-`, `_`, `.`, or `:`.

## Consistency and Failure Policy

- Session lookup failure returns HTTP 503; an absent session returns HTTP 401.
- Image list cache failure falls back to the filesystem and asynchronously attempts to refill Redis.
- Cache invalidation is best-effort after successful upload/delete. The short TTL bounds stale data if invalidation fails.
- Upload rate-limit, concurrent-permit, and idempotency failures return HTTP 503 before MySQL/filesystem work begins.
- MySQL's `(user_id or username, sha256)` uniqueness remains authoritative. Redis idempotency prevents repeated HTTP operations; it does not replace content-hash deduplication.

## Upload Admission Sequence

1. Validate the authenticated request and optional `Idempotency-Key` in the application layer.
2. If a key exists, atomically claim it with `SET ... NX EX`. A completed claim replays the stored JSON body; an active claim returns HTTP 409 with `Retry-After: 1`.
3. Atomically increment the per-minute rate counter using Lua. Over-limit requests return HTTP 429 and release a newly created idempotency claim.
4. Atomically acquire a concurrent permit using Lua. The script increments, installs a TTL, and rolls back the increment when the configured limit is exceeded.
5. Schedule existing MySQL/filesystem upload work on the worker pool.
6. Always release the concurrent permit. On success, store `DONE\n<json-body>` under the idempotency key; on failure/cancellation, delete only a still-`PROCESSING` claim.
7. On success, write image status `READY` and invalidate the global image-list key.

## Cache Behavior

- Image list values contain only the JSON response body, not HTTP headers.
- Image status values currently use `READY`; the later image-processing phase will add `RECEIVING`, `PENDING`, `PROCESSING`, and `FAILED` backed by MySQL.
- Redis cache values are bounded by existing HTTP/image limits and configured TTLs.

## Configuration

- `REDIS_KEY_PREFIX=myweb:v5:`
- `IMAGE_LIST_CACHE_TTL_SECONDS=3`
- `IMAGE_STATUS_CACHE_TTL_SECONDS=30`
- `UPLOAD_RATE_LIMIT_PER_MINUTE=20`
- `MAX_CONCURRENT_UPLOADS_PER_USER=2`
- `UPLOAD_PERMIT_TTL_SECONDS=30`
- `UPLOAD_IDEMPOTENCY_TTL_SECONDS=86400`

## Testing

- A real-Redis integration test validates binary-safe SET/GET/DEL, NX semantics, and integer Lua replies through the shared asynchronous client.
- A real-Redis integration test validates cache hit/miss/invalidation, rate-limit boundaries, concurrent permit acquisition/release, and idempotency claim/replay/abort.
- Existing session integration tests run unchanged against the extracted transport.
- HTTP helper tests validate idempotency-key syntax and cached-response behavior without asserting on mocks.
