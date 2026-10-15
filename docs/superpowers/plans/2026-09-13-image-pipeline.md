# Image Transfer and Processing Implementation Plan

**Goal:** Add client ET, bounded asynchronous thumbnails, direct-body upload
spooling, and Nginx offload for both originals and thumbnails.

**Architecture:** Keep socket ownership in each SubReactor. HttpConnection
drains ET sockets and spools direct upload bodies. Workers publish originals;
a separate bounded queue runs libvips. Redis holds transient processing state,
while the existing image volume holds durable files.

## Task 1: Enable and verify client ET

- [x] Define one client event policy combining EPOLLET and EPOLLONESHOT.
- [x] Verify pipelined requests survive a drain-to-EAGAIN read.
- [x] Build the server and focused test.

## Task 2: Add bounded asynchronous thumbnail processing

- [ ] Write queue admission, success, and failure tests.
- [ ] Implement ImageProcessor with fixed workers and bounded capacity.
- [ ] Enqueue after original publication; update Redis status on completion.
- [ ] Add thumbnail URLs, deletion cleanup, metrics, and shutdown.

## Task 3: Stream direct upload bodies to temporary files

- [ ] Write socketpair tests proving the request body is spooled and not stored.
- [ ] Extend HttpRequest with owned temporary-file metadata.
- [ ] Extend HttpConnection with header-first parsing and chunked file writes.
- [ ] Validate/hash/publish files without loading the whole file.
- [ ] Switch the bundled browser page to the direct-body protocol.

## Task 4: Complete Nginx and container integration

- [ ] Add protected thumbnail internal location and cache policy.
- [ ] Add libvips build/runtime dependency and processing configuration.
- [ ] Preserve the existing image volume and create private subdirectories.
- [ ] Validate Compose and Nginx configuration.

## Task 5: Full verification

- [ ] Run the complete CMake build and CTest suite.
- [ ] Run Redis-backed integration tests.
- [ ] Exercise upload, status, thumbnail, original, download, and deletion.
- [ ] Inspect metrics and document deployment/benchmark commands.
