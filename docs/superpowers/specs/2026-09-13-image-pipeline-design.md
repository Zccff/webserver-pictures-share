# Image Transfer and Processing Design

## Goal

Reduce upload memory amplification and thumbnail bandwidth while keeping the
SubReactor nonblocking. Original images remain private and are served by Nginx
only after the C++ application authorizes the request.

## Accepted architecture

The browser sends the file bytes directly rather than wrapping them in
multipart form data. HttpConnection parses the headers and streams the body
to a private temporary file while draining the ET socket to EAGAIN. The
worker receives only a file path and metadata, validates the signature, hashes
the file incrementally, reserves MySQL metadata, and atomically renames the
temporary file into the image directory.

After publication the request returns without waiting for image conversion. A
bounded image-processing queue invokes libvips workers to generate a WebP
thumbnail. Redis stores PROCESSING, READY, or FAILED; the image list is
invalidated after a transition. Queue saturation never becomes an unbounded
memory queue.

## Storage layout

    /app/webroot/images/
      <owner-prefix>_<sha256>.<ext>       original
      thumbnails/
        <owner-prefix>_<sha256>.webp      generated thumbnail
      .upload-tmp/
        <random>.part                     in-progress upload

The existing Docker image_data volume contains all three paths, so no new
volume or database is introduced.

## Transfer flow

1. Nginx applies the body-size limit and forwards the authenticated upload.
2. The SubReactor drains the nonblocking socket under EPOLLET and EPOLLONESHOT.
3. For direct image uploads, body chunks are written once to a temporary file.
4. A worker validates, hashes, performs the MySQL ownership/duplicate
   transaction, and atomically publishes the original.
5. The bounded image queue starts thumbnail generation and the HTTP request
   returns with thumbnail_status=PROCESSING.
6. libvips generates a bounded 480x480 WebP thumbnail and updates Redis.
7. The gallery loads the thumbnail; opening or downloading the image uses the
   original.
8. C++ authenticates original and thumbnail requests, then returns
   X-Accel-Redirect; Nginx performs file transfer and slow-client backpressure.

## Compatibility and failures

- The old multipart endpoint remains supported during rollout, but the bundled
  page uses the streaming direct-body format.
- Temporary files are deleted on disconnect, parse failure, duplicate upload,
  database failure, and shutdown.
- A thumbnail failure does not delete the valid original. It is visible as
  FAILED, and the UI falls back to the original preview.
- Redis failure does not prevent serving an already-published original.
- MySQL remains authoritative for ownership, per-user count, and duplicate
  content hashes.

## Configuration

- UPLOAD_TEMP_DIRECTORY defaults to <webroot>/images/.upload-tmp.
- IMAGE_PROCESSOR_THREADS defaults to 2.
- IMAGE_PROCESSOR_QUEUE_CAPACITY defaults to 256.
- THUMBNAIL_MAX_WIDTH and THUMBNAIL_MAX_HEIGHT default to 480.
- THUMBNAIL_WEBP_QUALITY defaults to 82.

## Verification

Tests cover ET event flags and pipelined reads, bounded image-queue admission,
success/failure callbacks, direct-body spooling without retaining the body in
HttpRequest, temporary-file cleanup, and HTTP thumbnail URLs. The Docker image
is built with libvips and the Nginx configuration is syntax-checked.
