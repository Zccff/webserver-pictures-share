# Shared Async Redis Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Share one nonblocking Redis pool across sessions, image caches, upload limits, and upload idempotency.

**Architecture:** Extract the hiredis/libuv transport from the session domain into `AsyncRedisClient`. Build `AsyncSessionManager` and `ImageRedisService` as typed adapters, then compose image Redis operations in the existing asynchronous HTTP response path.

**Tech Stack:** C++17, hiredis asynchronous API, libuv, Redis 7 Lua scripts, CMake/CTest, WSL2.

**Spec:** `docs/superpowers/specs/2026-09-13-redis-platform-design.md`

## Global Constraints

- Redis I/O must never block a SubReactor or worker thread.
- One bounded hiredis/libuv pool is shared by every Redis-backed feature.
- `REDIS_COMMAND_CAPACITY` defaults to 4096 and `REDIS_MAX_PIPELINE_PER_CONNECTION` defaults to 256.
- Session, admission, and idempotency errors fail closed; image cache errors fail open.
- MySQL remains authoritative for image ownership, count, and content-hash uniqueness.
- Existing v5 uncommitted changes must be preserved, and no Git operation is performed.

---

### Task 1: Extract the shared asynchronous Redis transport

**Files:**
- Create: `include/redis/AsyncRedisClient.h`
- Create: `src/redis/AsyncRedisClient.cpp`
- Create: `tests/async_redis_client_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `AsyncRedisClient::instance()`, `init_from_env()`, `shutdown()`, `available()`, `status_message()`, and `command(std::vector<std::string>, Callback)`.
- Produces: `AsyncRedisResult` with `RedisResultType::{Status,String,Integer,Nil,Error}`, `value`, `integer`, and `error`.

- [ ] **Step 1: Write the failing transport integration test**

```cpp
client.command({"SETEX", key, "30", "value\\0with-binary"}, callback);
client.command({"GET", key}, callback);
client.command({"SET", nx_key, "first", "NX", "EX", "30"}, callback);
client.command({"EVAL", "return 7", "0"}, callback);
```

- [ ] **Step 2: Run the focused build and verify RED**

Run: `cmake --build build --target async_redis_client_test -j2`

Expected: compilation fails because `redis/AsyncRedisClient.h` and the target do not exist.

- [ ] **Step 3: Implement the minimal shared client**

Implement one libuv thread, a bounded MPSC command queue, least-outstanding ready-connection selection, argv/length binary-safe dispatch, timeouts, reconnects, and shutdown callbacks.

- [ ] **Step 4: Run the focused integration test and verify GREEN**

Run: `RUN_REDIS_INTEGRATION_TESTS=1 ctest --test-dir build -R async_redis_client_test --output-on-failure`

Expected: PASS against Redis at `REDIS_HOST`/`REDIS_PORT`; without the opt-in environment it exits 77 and CTest marks it skipped.

### Task 2: Migrate Session to the shared transport

**Files:**
- Modify: `include/auth/AsyncSessionManager.h`
- Modify: `src/auth/AsyncSessionManager.cpp`
- Modify: `tests/async_session_manager_test.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `AsyncRedisClient::command`.
- Preserves: `create_session`, `get_session`, and `destroy_session` callback contracts.

- [ ] **Step 1: Extend the existing session integration test to check prefixed keys and binary-safe display names**

```cpp
manager.create_session("async_test_user", "Async\\nDisplay", callback);
manager.get_session(token, callback);
```

- [ ] **Step 2: Run the test against the unmodified adapter and verify RED**

Expected: the embedded newline cannot round-trip through the current newline-delimited payload.

- [ ] **Step 3: Replace the private Redis transport with the shared client and length-prefix the session payload**

Encode payload as `<username-size>:<username><display-name>` and use the configured Redis key prefix.

- [ ] **Step 4: Run both Redis transport and session tests and verify GREEN**

Run: `RUN_REDIS_INTEGRATION_TESTS=1 ctest --test-dir build -R 'async_(redis_client|session_manager)_test' --output-on-failure`

Expected: both tests pass.

### Task 3: Add typed image Redis operations

**Files:**
- Create: `include/cache/ImageRedisService.h`
- Create: `src/cache/ImageRedisService.cpp`
- Create: `tests/image_redis_service_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: cache `get/put/invalidate` operations for list and status.
- Produces: `check_upload_rate`, `acquire_upload_permit`, `release_upload_permit`.
- Produces: `begin_upload`, `complete_upload`, and `abort_upload` idempotency operations.

- [ ] **Step 1: Write failing integration cases for every boundary**

Use literal expectations: third permit is rejected when limit is two; the third request is rejected when the rate is two; a second idempotency claim observes `PROCESSING`; completion replays the exact stored body; abort permits a fresh claim.

- [ ] **Step 2: Build and verify RED because `ImageRedisService` does not exist**

Run: `cmake --build build --target image_redis_service_test -j2`

- [ ] **Step 3: Implement typed methods using binary-safe Redis argv and atomic Lua scripts**

Every callback distinguishes a legitimate cache miss from Redis failure. Release and abort scripts never drive counters below zero or delete completed results.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run: `RUN_REDIS_INTEGRATION_TESTS=1 ctest --test-dir build -R image_redis_service_test --output-on-failure`

### Task 4: Compose Redis admission and caching into HTTP image routes

**Files:**
- Modify: `include/http/HttpResponse.h`
- Modify: `src/http/HttpResponse.cpp`
- Create: `tests/http_redis_helpers_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: all `ImageRedisService` operations and the existing `Executor` worker rescheduling interface.
- Produces: asynchronous GET `/api/images` cache hit/miss path and asynchronous POST `/api/upload-image` admission path.

- [ ] **Step 1: Write failing tests for idempotency-key validation and replay body extraction**

Literal cases include valid UUID-like keys, empty optional keys, control characters, spaces, seven-character keys, and 129-character keys.

- [ ] **Step 2: Run and verify RED because the helper contract is absent**

Run: `cmake --build build --target http_redis_helpers_test -j2`

- [ ] **Step 3: Implement the asynchronous route composition**

Cache hits finish directly; misses schedule filesystem/MySQL work. New uploads claim idempotency, check rate, acquire a permit, schedule existing upload work, release on every terminal path, update status to `READY`, invalidate the list, and complete or abort idempotency.

- [ ] **Step 4: Run focused and full tests and verify GREEN**

Run: `ctest --test-dir build --output-on-failure`

### Task 5: Add deployment configuration, metrics, and operational documentation

**Files:**
- Modify: `docker-compose.yml`
- Modify: `deploy/offline/docker-compose.yml`
- Modify: `deploy/offline/.env.example`
- Modify: `include/utils/Metrics.h`
- Modify: `src/utils/Metrics.cpp`
- Modify: `docs/async-redis.md`

**Interfaces:**
- Produces: documented environment variables and metrics for Redis image-cache hits/misses, rate rejects, permit rejects, idempotency replays, and Redis command rejects.

- [ ] **Step 1: Add behavior tests for new metric counters**

Extend a focused C++ test to increment each metric and assert parsed JSON numeric values.

- [ ] **Step 2: Verify RED because the counters are absent**

Run: `cmake --build build --target metrics_test -j2`

- [ ] **Step 3: Implement counters and update deployment files**

Use the exact defaults from the design spec and keep secrets out of Redis keys and logs.

- [ ] **Step 4: Run full verification**

Run: `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build -j2 && ctest --test-dir build --output-on-failure`

Expected: build succeeds; unit tests pass; Redis integration tests either pass when opted in or are explicitly skipped with exit code 77.
