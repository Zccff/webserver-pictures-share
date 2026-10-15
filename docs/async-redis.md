# 异步 Redis 会话层

## 设计结论

当前生效的 Redis 会话实现是 `AsyncSessionManager`。它参考 redis-plus-plus
异步接口的核心模型，使用 hiredis async 负责 Redis 协议和非阻塞连接，使用
libuv 独立事件循环驱动全部 Redis fd。

它不是“在线程池中执行同步 Redis”，也没有在 Worker 中调用 `future.get()`。

```text
Worker / Reactor caller
        |
        | submit command (never waits for Redis)
        v
bounded MPSC command queue --uv_async_send--> Redis libuv thread
                                                |
                                                v
                                    hiredis async connection pool
                                      + automatic pipelining
                                                |
                                         reply callback
                                                |
                                 submit business continuation
                                                v
                                         Worker pool
                                                |
                                  completion queue + eventfd
                                                v
                                         SubReactor send
```

## 关键约束

- Redis 连接只能由 libuv 线程访问。hiredis async context 不是线程安全对象，
  Worker 只能向有界 MPSC 队列提交命令。
- Redis 回调中不执行文件、MySQL、图片哈希等慢操作，只把后续业务重新投递
  给 Worker。
- 每个连接允许有限深度的 pipeline；调度器选择未满且当前 pipeline 最短的
  连接。
- `REDIS_COMMAND_CAPACITY` 同时限制等待和在途命令，满载时快速失败，防止
  Redis 故障期间无限积压。
- hiredis 的连接和命令超时由 libuv timer 驱动；断线后定时尝试修复连接。
- Session Cache 命中时可直接完成，不进入 Redis 事件循环。
- 客户端连接关闭后，连接取消标记会阻止 Redis 回调继续构造或回写响应。

## WSL 依赖和构建

在 Ubuntu/WSL 中安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  default-libmysqlclient-dev libhiredis-dev libuv1-dev
```

构建：

```bash
cd /mnt/d/TinyLiunxWeb/MyWebServer
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
```

异步会话测试需要一个 Redis。测试覆盖 SETEX、GET、DEL 和 512 个并发提交的
GET：

```bash
redis-server --port 6389 --save "" --appendonly no --daemonize yes
RUN_REDIS_INTEGRATION_TESTS=1 REDIS_PORT=6389 SESSION_CACHE_TTL_SECONDS=0 \
  ./build/async_session_manager_test
redis-cli -p 6389 shutdown nosave
```

## 配置

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `REDIS_POOL_SIZE` | 8 | 异步长连接数 |
| `REDIS_IO_TIMEOUT_MS` | 3000 | 连接与命令超时 |
| `REDIS_COMMAND_CAPACITY` | 4096 | 等待加在途命令总上限 |
| `REDIS_MAX_PIPELINE_PER_CONNECTION` | 256 | 每条连接最大 pipeline 深度 |
| `SESSION_CACHE_TTL_SECONDS` | 3 | 进程内 Session Cache TTL |
| `SESSION_CACHE_CAPACITY` | 20000 | 本地 Session 条目上限 |

## 和原同步池的区别

原实现的每条连接有 mutex，Worker 在持有 mutex 时调用阻塞式 `send/recv`。
连接复用是正确的，但 Redis 延迟会直接占用 Worker。

新实现没有每连接业务锁：全部连接归单一 libuv 线程所有，命令通过队列跨线程
提交，并由回调续接。Redis 变慢会增加异步在途请求和响应延迟，但不会让 Worker
停在 `recv` 上；达到命令上限后返回 503，以保护进程内存和其他接口。

## 面试回答

可以这样描述：

> 我最初实现的是同步 Redis 长连接池，虽然没有阻塞 Reactor，但会阻塞 Worker。
> 后来参考 redis-plus-plus 的异步模型，改为 hiredis async + libuv 独立事件循环。
> Worker 通过有界 MPSC 队列提交命令，连接由事件循环单线程持有，多命令自动
> pipeline，reply callback 只负责把业务 continuation 投回 Worker，绝不在 I/O
> 回调里执行慢业务。这样 Redis 延迟不再占住 Worker，同时通过命令总量、单连接
> pipeline 深度和超时限制实现背压。

异步化不会让 Redis 本身变快；它解决的是等待期间的线程占用。Redis 服务端饱和、
热点 key、网络带宽或命令复杂度仍然会限制吞吐，因此必须同时观察 Redis 延迟、
在途命令数、队列拒绝数、Session Cache 命中率和 HTTP p99。
