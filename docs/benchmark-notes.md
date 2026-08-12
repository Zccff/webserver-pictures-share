# Benchmark Notes

## Run 1

- Target: `http://127.0.0.1:8080/`
- Duration: `30s`
- Threads: `12`
- Connections: `500`
- Total requests: `305939`
- Total read: `4.54GB`
- Requests/sec: `10166.10`
- Transfer/sec: `154.44MB`
- Average latency: `50.24ms`
- Latency stdev: `31.47ms`
- Max latency: `540.08ms`
- Latency p50: `48.69ms`
- Latency p75: `59.68ms`
- Latency p90: `68.90ms`
- Latency p99: `135.04ms`
- Thread Req/Sec avg: `0.86k`
- Thread Req/Sec stdev: `179.32`
- Thread Req/Sec max: `1.30k`
- Notes: Root page test through local nginx port `8080`.

## Run 2

- Target: `http://127.0.0.1:9006/`
- Location: inside `mywebserver-app` container
- Duration: `30s`
- Threads: `12`
- Connections: `500`
- Total requests: `351633`
- Total read: `5.16GB`
- Requests/sec: `11683.96`
- Transfer/sec: `175.60MB`
- Average latency: `41.63ms`
- Latency stdev: `8.88ms`
- Max latency: `137.01ms`
- Latency p50: `41.80ms`
- Latency p75: `46.09ms`
- Latency p90: `48.66ms`
- Latency p99: `74.07ms`
- Thread Req/Sec avg: `0.98k`
- Thread Req/Sec stdev: `197.07`
- Thread Req/Sec max: `1.60k`
- Notes: Root page test direct to C++ app on port `9006`, bypassing nginx.

## Run 3

- Target: `http://127.0.0.1:9006/`
- Location: inside `mywebserver-app` container
- Label: second local direct run
- Duration: `30s`
- Threads: `12`
- Connections: `500`
- Total requests: `370231`
- Total read: `5.43GB`
- Requests/sec: `12303.30`
- Transfer/sec: `184.91MB`
- Average latency: `39.75ms`
- Latency stdev: `9.42ms`
- Max latency: `135.49ms`
- Latency p50: `38.31ms`
- Latency p75: `44.51ms`
- Latency p90: `48.34ms`
- Latency p99: `77.03ms`
- Thread Req/Sec avg: `1.03k`
- Thread Req/Sec stdev: `215.05`
- Thread Req/Sec max: `1.76k`
- Notes: Same root page direct-to-app test as Run 2, second iteration.

## Run 4

- Target: `http://127.0.0.1:8080/api/me`
- Path: `/api/me`
- Via: nginx port `8080`
- Cookie: `sid=3734494ca013a45a3ea6856837687d1a6b280b73df6a300406e9e6a741034132`
- Duration: `30s`
- Threads: `4`
- Connections: `100`
- Total requests: `95494`
- Total read: `24.59MB`
- Requests/sec: `3179.25`
- Transfer/sec: `838.27KB`
- Average latency: `31.64ms`
- Latency stdev: `9.38ms`
- Max latency: `148.33ms`
- Latency p50: `30.04ms`
- Latency p75: `34.09ms`
- Latency p90: `40.09ms`
- Latency p99: `72.12ms`
- Thread Req/Sec avg: `799.26`
- Thread Req/Sec stdev: `151.13`
- Thread Req/Sec max: `1.34k`
- Notes: Authenticated `/api/me` test through nginx. Exercises cookie parsing and Redis session lookup.
- Resource snapshot during run:
  - System load: `1.56`
  - Processes: `131`
  - nginx CPU: `46.07%`, memory: `6.473MiB / 1.571GiB`, net I/O: `136MB / 5.05GB`, block I/O: `3.09MB / 4.1kB`, PIDs: `3`
  - app CPU: `68.19%`, memory: `41.28MiB / 1.571GiB`, net I/O: `53.8MB / 73.2MB`, block I/O: `38.5MB / 6.46MB`, PIDs: `10`
  - redis CPU: `21.53%`, memory: `11.72MiB / 1.571GiB`, net I/O: `23.9MB / 13.5MB`, block I/O: `15.7MB / 0B`, PIDs: `6`
  - mysql CPU: `0.65%`, memory: `440.3MiB / 1.571GiB`, net I/O: `6.41kB / 7.81kB`, block I/O: `71.8MB / 17.3MB`, PIDs: `36`

## Run 5

- Target: `http://127.0.0.1:8080/images/city-street.png`
- Path: `/images/city-street.png`
- Via: nginx port `8080`
- Cookie: `sid=3734494ca013a45a3ea6856837687d1a6b280b73df6a300406e9e6a741034132`
- Duration: `30s`
- Threads: `4`
- Connections: `100`
- Total requests: `1381`
- Total read: `3.24GB`
- Requests/sec: `45.94`
- Transfer/sec: `110.32MB`
- Average latency: `1.74s`
- Latency stdev: `285.09ms`
- Max latency: `2.00s`
- Latency p50: `1.83s`
- Latency p75: `1.91s`
- Latency p90: `1.97s`
- Latency p99: `1.99s`
- Thread Req/Sec avg: `15.38`
- Thread Req/Sec stdev: `11.25`
- Thread Req/Sec max: `90.00`
- Socket errors: connect `0`, read `0`, write `0`, timeout `807`
- Notes: Authenticated protected image test through nginx. Exercises cookie parsing, Redis session lookup, LRU/file read, and large response transfer.
- Resource snapshot during run:
  - Timestamp: `Sun Jul 5 12:33:57 PM CST 2026`
  - System load: `1.56`
  - Processes: `131`
  - nginx CPU: `40.55%`, memory: `72.08MiB / 1.571GiB`, net I/O: `2.72GB / 7.59GB`, block I/O: `3.17MB / 42.6MB`, PIDs: `3`
  - app CPU: `64.56%`, memory: `91.22MiB / 1.571GiB`, net I/O: `70MB / 2.65GB`, block I/O: `38.5MB / 12.6MB`, PIDs: `10`
  - redis CPU: `9.86%`, memory: `12.99MiB / 1.571GiB`, net I/O: `29.6MB / 16.7MB`, block I/O: `18.1MB / 0B`, PIDs: `6`
  - mysql CPU: `0.98%`, memory: `440.9MiB / 1.571GiB`, net I/O: `6.41kB / 7.81kB`, block I/O: `74.9MB / 17.3MB`, PIDs: `36`

## Run 6

- Target: `http://127.0.0.1:8080/api/login`
- Intended script: `login.lua`
- Via: nginx port `8080`
- Duration: `30s`
- Threads: `4`
- Connections: `50`
- Total requests: `158475`
- Total read: `34.61MB`
- Requests/sec: `5276.35`
- Transfer/sec: `1.15MB`
- Average latency: `9.30ms`
- Latency stdev: `4.82ms`
- Max latency: `65.47ms`
- Latency p50: `8.47ms`
- Latency p75: `11.54ms`
- Latency p90: `15.14ms`
- Latency p99: `25.75ms`
- Thread Req/Sec avg: `1.33k`
- Thread Req/Sec stdev: `373.38`
- Thread Req/Sec max: `2.95k`
- Non-2xx or 3xx responses: `158475`
- Script error: `login.lua: cannot open login.lua: No such file or directory`
- Notes: Invalid login benchmark. The Lua script did not load, so wrk likely sent default requests rather than intended POST form login requests. All responses were non-2xx/3xx; exclude from login bottleneck conclusions.
- Resource snapshot during run:
  - System load: `1.56`
  - Processes: `131`
  - nginx CPU: `60.26%`, memory: `8.137MiB / 1.571GiB`, net I/O: `3.77GB / 8.65GB`, block I/O: `3.17MB / 43.6MB`, PIDs: `3`
  - app CPU: `57.16%`, memory: `66.33MiB / 1.571GiB`, net I/O: `91.4MB / 3.68GB`, block I/O: `38.5MB / 14.7MB`, PIDs: `10`
  - redis CPU: `0.89%`, memory: `13.04MiB / 1.571GiB`, net I/O: `29.7MB / 16.7MB`, block I/O: `18.1MB / 0B`, PIDs: `6`
  - mysql CPU: `11.81%`, memory: `440.9MiB / 1.571GiB`, net I/O: `6.41kB / 7.81kB`, block I/O: `74.9MB / 17.3MB`, PIDs: `36`

## Run 7

- Target: `http://127.0.0.1:8080/api/login`
- Path: `/api/login`
- Via: nginx port `8080`
- Script: `login.lua`
- Duration: `30s`
- Threads: `4`
- Connections: `50`
- Total requests: `16810`
- Total read: `6.57MB`
- Requests/sec: `559.53`
- Transfer/sec: `224.03KB`
- Average latency: `85.62ms`
- Latency stdev: `13.16ms`
- Max latency: `178.01ms`
- Latency p50: `81.98ms`
- Latency p75: `86.46ms`
- Latency p90: `99.20ms`
- Latency p99: `137.57ms`
- Thread Req/Sec avg: `140.53`
- Thread Req/Sec stdev: `20.56`
- Thread Req/Sec max: `200.00`
- Notes: Valid login benchmark through nginx. Exercises POST parsing, MySQL password lookup, Redis session creation, and Set-Cookie response.
- Resource snapshot during run:
  - Timestamp: `Sun Jul 5 12:33:57 PM CST 2026`
  - System load: `1.56`
  - Processes: `131`
  - nginx CPU: `25.50%`, memory: `8.332MiB / 1.571GiB`, net I/O: `3.79GB / 8.67GB`, block I/O: `3.17MB / 44MB`, PIDs: `3`
  - app CPU: `47.09%`, memory: `66.96MiB / 1.571GiB`, net I/O: `106MB / 3.7GB`, block I/O: `39.5MB / 24.1MB`, PIDs: `10`
  - redis CPU: `11.20%`, memory: `14.07MiB / 1.571GiB`, net I/O: `30.6MB / 17MB`, block I/O: `18.2MB / 24.6kB`, PIDs: `6`
  - mysql CPU: `60.87%`, memory: `442.5MiB / 1.571GiB`, net I/O: `2.08MB / 2.96MB`, block I/O: `77MB / 17.3MB`, PIDs: `36`

# Optimized Benchmark Notes

## Optimized Run A

- Target: `http://127.0.0.1:8080/`
- Path: `/`
- Via: nginx port `8080`
- Duration: `30s`
- Threads: `12`
- Connections: `500`
- Total requests: `300583`
- Total read: `4.46GB`
- Requests/sec: `9992.29`
- Transfer/sec: `151.80MB`
- Average latency: `49.57ms`
- Max latency: `284.35ms`
- Latency p50: `48.47ms`
- Latency p75: `59.31ms`
- Latency p90: `69.76ms`
- Latency p99: `109.77ms`
- Resource snapshot: nginx CPU `73.00%`, app CPU `0.00%`, redis CPU `5.21%`, mysql CPU `1.55%`
- Notes: Root page served directly by nginx static file path.

## Optimized Run B

- Target: `http://127.0.0.1:9006/`
- Path: `/`
- Location: inside `mywebserver-app` container
- Duration: `30s`
- Threads: `12`
- Connections: `500`
- Total requests: `350677`
- Total read: `5.15GB`
- Requests/sec: `11650.74`
- Transfer/sec: `175.10MB`
- Average latency: `41.93ms`
- Max latency: `158.55ms`
- Latency p50: `39.28ms`
- Latency p75: `46.89ms`
- Latency p90: `53.56ms`
- Latency p99: `83.35ms`
- Resource snapshot: nginx CPU `0.00%`, app CPU `170.26%`, redis CPU `0.79%`, mysql CPU `0.71%`
- Notes: Direct C++ app static root page benchmark.

## Optimized Run C

- Target: `http://127.0.0.1:8080/api/me`
- Via: nginx port `8080`
- Cookie: `sid=b3e6e4993026c8695a30fd8b39885f702909e2f5cee3b60bf1f9c7a7281346c2`
- Duration: `30s`
- Threads: `4`
- Connections: `100`
- Total requests: `176294`
- Total read: `45.39MB`
- Requests/sec: `5868.10`
- Transfer/sec: `1.51MB`
- Average latency: `17.29ms`
- Max latency: `153.83ms`
- Latency p50: `16.14ms`
- Latency p75: `20.89ms`
- Latency p90: `26.07ms`
- Latency p99: `41.34ms`
- Resource snapshot: nginx CPU `56.51%`, app CPU `57.61%`, redis CPU `0.91%`, mysql CPU `9.06%`
- Notes: Authenticated session check after Redis pool and local session cache.

## Optimized Run D

- Target: `http://127.0.0.1:8080/api/images`
- Via: nginx port `8080`
- Cookie: `sid=b3e6e4993026c8695a30fd8b39885f702909e2f5cee3b60bf1f9c7a7281346c2`
- Duration: `30s`
- Threads: `4`
- Connections: `100`
- Total requests: `99595`
- Total read: `90.90MB`
- Requests/sec: `3316.80`
- Transfer/sec: `3.03MB`
- Average latency: `30.33ms`
- Max latency: `191.67ms`
- Latency p50: `29.20ms`
- Latency p75: `35.12ms`
- Latency p90: `42.80ms`
- Latency p99: `66.42ms`
- Resource snapshot: nginx CPU `42.98%`, app CPU `80.63%`, redis CPU `5.42%`, mysql CPU `0.67%`
- Notes: Authenticated image list; app scans image directory and builds JSON.

## Optimized Run E

- Target: `http://127.0.0.1:8080/images/city-street.png`
- Via: nginx port `8080`
- Cookie: `sid=b3e6e4993026c8695a30fd8b39885f702909e2f5cee3b60bf1f9c7a7281346c2`
- Duration: `30s`
- Threads: `4`
- Connections: `100`
- Total requests: `7623`
- Total read: `17.91GB`
- Requests/sec: `253.28`
- Transfer/sec: `609.41MB`
- Average latency: `396.21ms`
- Max latency: `1.54s`
- Latency p50: `377.66ms`
- Latency p75: `523.61ms`
- Latency p90: `671.46ms`
- Latency p99: `951.46ms`
- Resource snapshot: nginx CPU `45.76%`, app CPU `10.25%`, redis CPU `1.01%`, mysql CPU `0.85%`
- Notes: Protected image served via C++ auth plus nginx `X-Accel-Redirect`/sendfile.

## Optimized Run F

- Target: `http://127.0.0.1:8080/images/city-street.png`
- Via: nginx port `8080`
- Cookie: `sid=b3e6e4993026c8695a30fd8b39885f702909e2f5cee3b60bf1f9c7a7281346c2`
- Duration: `30s`
- Threads: `4`
- Connections: `10`
- Total requests: `9672`
- Total read: `22.63GB`
- Requests/sec: `321.48`
- Transfer/sec: `770.26MB`
- Average latency: `24.24ms`
- Max latency: `175.09ms`
- Latency p50: `22.19ms`
- Latency p75: `29.37ms`
- Latency p90: `37.98ms`
- Latency p99: `71.94ms`
- Notes: Low-concurrency protected image benchmark.

## Optimized Run G

- Target: `http://127.0.0.1:8080/api/login`
- Via: nginx port `8080`
- Script: `login.lua`
- Duration: `30s`
- Threads: `4`
- Connections: `50`
- Total requests: `25816`
- Total read: `10.09MB`
- Requests/sec: `859.63`
- Transfer/sec: `344.19KB`
- Average latency: `55.78ms`
- Max latency: `143.42ms`
- Latency p50: `53.96ms`
- Latency p75: `58.43ms`
- Latency p90: `66.65ms`
- Latency p99: `88.06ms`
- Resource snapshot: nginx CPU `19.92%`, app CPU `57.68%`, redis CPU `14.36%`, mysql CPU `67.63%`
- Notes: Valid login benchmark after MySQL pool and Redis pool changes.

## Optimized Run H

- Target: `http://127.0.0.1:8080/api/login`
- Via: nginx port `8080`
- Script: `login.lua`
- Duration: `30s`
- Threads: `4`
- Connections: `200`
- Total requests: `25840`
- Total read: `10.10MB`
- Requests/sec: `859.86`
- Transfer/sec: `344.28KB`
- Average latency: `231.47ms`
- Max latency: `497.92ms`
- Latency p50: `222.39ms`
- Latency p75: `236.22ms`
- Latency p90: `271.97ms`
- Latency p99: `331.48ms`
- Notes: Higher-concurrency login benchmark. Throughput is almost identical to Optimized Run G at 50 connections, while latency rises sharply.
