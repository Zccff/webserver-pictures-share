# 异常与高并发加固说明

## 这次解决了什么

| 风险 | 当前实现 | 满载时的行为 |
|---|---|---|
| Worker 队列无限增长 | `WORKER_QUEUE_CAPACITY`，默认 4096 | 返回 503，不继续堆内存 |
| 新连接和 fd 分发无限增长 | 每个 Sub Reactor 的连接总量与 pending 队列分别设上限 | 关闭新连接并累计拒绝指标 |
| 回写任务无限增长 | 每个 Sub Reactor 的 completion 队列设上限 | 对 Worker 施加背压 |
| Redis 故障拖死 Worker | hiredis async + libuv 独立事件循环，有界命令队列和 pipeline | 队列满或超时返回 503 |
| 日志风暴占满内存 | 有界日志队列 | 丢弃新日志，记录累计丢弃数量 |
| MySQL 池耗尽后轮询忙连接 | CAS 扫描空闲健康连接，失败后条件变量定时等待 | 100ms 默认超时，接口返回 503 |
| 持池锁修复连接形成自锁 | 离开池互斥区后再执行修复 | 修复路径可重新更新池状态 |
| fd 关闭了但业务继续写回 | fd generation + 每连接 cancellation token | 旧完成任务不能操作复用后的 fd |
| Keep-Alive 不断产生过期定时器节点 | fd 到堆下标的索引最小堆，原地 adjust/remove | 定时器内存按连接数而非请求数增长 |
| 连接超时但写操作仍提交 | idle timeout 与 business timeout 分离，事务提交前检查取消 | 未提交上传/注册/删除回滚 |
| 同一用户并发上传过多 | Redis ZSET 租约信号量，每次上传使用唯一 lease token | 返回 429；旧请求不能误释放新租约 |
| 单用户图片无限增长 | 上传事务锁住用户行，再统计并写入，默认 100 张 | 返回 429 |
| 同图换文件名重复上传 | 对内容计算 SHA-256，数据库唯一键 `(owner_username, content_hash)` | 返回 409 |
| 越权删除图片 | Session 鉴权后在数据库核对 `owner_username` | 返回 401/403/404 |
| 删除时数据库与文件不一致 | 原图先移入隔离区，Redis 取得带 token 的删除栅栏，MySQL 提交后再清派生数据 | 提交前失败恢复原图和原状态；崩溃残留在启动时按 MySQL 结果恢复或清除 |
| 列表缓存失效后被旧查询回填 | Redis 列表版本号 + Lua compare-and-set | 旧版本查询结果不会覆盖新状态 |
| 缩略图子进程卡死 | `waitpid(WNOHANG)` + 期限，先 TERM 后 KILL | 默认 15 秒后终止，不永久占用图片 Worker |
| 依赖数据库报错校验输入 | 应用层检查字段长度、字符、邮箱格式、图片扩展名、magic bytes 和大小 | 返回明确的 400/413/415 |

数据库唯一键、外键和字段长度仍然保留。应用层校验负责友好响应，数据库约束负责
并发竞争下的最终正确性；二者不是替代关系。

## 上传与删除的并发语义

Nginx 先通过内部鉴权子请求确认 Session，再允许请求体流入 app；同时施加 IP 和
Session 两级上传限速。应用取得 Redis 用户级租约后，校验图片 magic bytes、计算
SHA-256。幂等键与该 SHA-256 绑定，因此同一键不能被另一个文件复用。MySQL 事务
通过 `SELECT users ... FOR UPDATE` 串行化同一用户的“统计
数量 + 插入元数据”，所以即使有多个应用实例，也不会同时越过图片数量上限。
唯一键保证两个并发的相同内容最终只有一个成功。元数据提交后，文件通过临时文件
加原子 rename 发布；写文件失败会补偿删除元数据。

每次上传先取得唯一 upload token，把图片生命周期置为 `UPLOADING`；只有同一 token
才能原子推进到 `ACTIVE`，缩略图回调也必须携带该代 token。这样即使异步 Redis
连接乱序，旧上传或旧缩略图也不能覆盖新一代图片状态。

删除必须先有有效 Session。Worker 查询所有权后，用同文件系统 `rename()` 将原图
移入 `.deleting` 隔离目录，再用唯一 delete token 把生命周期置为持久的
`DELETING`。这个阶段只失效列表并阻止新热度、状态和旧上传激活，不提前删除原有榜单
与状态；所以 MySQL 失败时恢复文件并取消栅栏即可完整回滚。MySQL 提交成功后才将
生命周期推进到 `DELETED`，同时清理点击榜、下载榜和缩略图状态。若 COMMIT 因断线
结果不确定，会用新连接读所有权决定继续提交还是回滚，不能凭错误码误恢复文件。
文件或缓存最终清理失败返回 202，并保持图片不可见。进程启动时扫描 `.deleting`：
MySQL 行仍存在则恢复原图与生命周期，行已不存在则清除隔离文件和 Redis 派生数据。

Redis 租约使用 Redis 服务器时间计算过期分数，并按唯一 token 释放，适用于多个 app
实例。永久配额和重复判断仍由 MySQL 事务与唯一键兜底。

## 超时不等于强行终止线程

业务请求提交 Worker 后，Sub Reactor 把 60 秒连接空闲定时器切换为默认 15 秒
业务定时器。超时回调只在所属 Sub Reactor 执行：关闭 fd，并把共享原子取消标记
设为 true。Worker 和异步 continuation 在关键阶段检查该标记，写事务在 commit
前回滚。

这是 cooperative cancellation（协作式取消），不是杀线程。MySQL C API 已经进入
一次阻塞 `recv` 后不能安全地从另一线程强停，因此连接还设置 2 秒 connect timeout
与 3 秒 read/write timeout。已经成功提交的不可逆副作用也不能因为客户端断开就
假装从未发生；需要幂等键、事务状态机或补偿操作处理。

## 默认容量与调参入口

| 配置 | 默认值 |
|---|---:|
| `LISTEN_BACKLOG` | 1024 |
| `MAX_CONNECTIONS_PER_SUB_REACTOR` | 4096 |
| `PENDING_CONNECTION_QUEUE_CAPACITY` | 1024 |
| `WORKER_QUEUE_CAPACITY` | 4096 |
| `COMPLETION_QUEUE_CAPACITY` | 4096 |
| `LOG_QUEUE_CAPACITY` | 8192 |
| `REDIS_COMMAND_CAPACITY` | 4096 |
| `REDIS_MAX_PIPELINE_PER_CONNECTION` | 256 |
| `MYSQL_POOL_WAIT_TIMEOUT_MS` | 100 |
| `CONNECTION_IDLE_TIMEOUT_MS` | 60000 |
| `BUSINESS_TIMEOUT_MS` | 15000 |
| `MAX_CONCURRENT_UPLOADS_PER_USER` | 2 |
| `MAX_IMAGES_PER_USER` | 100 |
| `UPLOAD_RATE_LIMIT_PER_MINUTE` | 20 |
| `IMAGE_LIFECYCLE_PENDING_TTL_SECONDS` | 120 |
| `IMAGE_PROCESSOR_TIMEOUT_SECONDS` | 15 |

这些数字不是越大越好。容量应根据可接受内存、下游吞吐和延迟目标计算，并同时观察
p95/p99、队列深度、拒绝数、CPU、RSS、MySQL 活跃连接和 Redis 在途命令。`listen`
backlog 只是内核中等待 `accept` 的已完成连接队列提示值，还会被 `somaxconn` 截断；
它不是应用层最大并发连接数。

## 面试问答

**为什么队列满时返回 503，而不是继续等待？**

有界队列是过载保护。无限等待会把延迟和内存一起放大，最终导致超时雪崩。503 加
`Retry-After` 让上游限速或重试，并保证健康请求仍有机会完成。

**为什么 completion 队列满时可以阻塞 Worker？**

这是从网络回写端向业务端传播背压，Sub Reactor 会很快交换并清空该队列。特别处理
了 Reactor 自己提交 Worker 失败的路径：它直接生成 503，不能等待自己负责清空的
completion 队列，否则会自锁。生产中也可以改成无等待拒绝加关闭连接。

**MySQL 连接是线程固定持有的吗？**

不是。每个请求动态租用一个空闲连接，完成后归还。CAS 只负责把 slot 的 `in_use`
从 false 改成 true；Prepared Statement 属于连接，因此每个连接各准备一套。池耗尽
时等待条件变量，不会盯着某条忙连接轮询。

**为什么图片数量限制要锁用户行？**

单独执行 `COUNT` 再 `INSERT` 会发生 check-then-act 竞争，两个请求都可能看到 99。
锁定同一用户行后，同一用户的这段事务串行执行；不同用户仍能并发。

**为什么既算 SHA-256，又建唯一索引？**

hash 用于把内容相同映射成相同摘要，唯一索引负责在并发和多实例条件下做原子裁决。
只在内存里“先查再写”仍有竞态。这里是去重，不是密码存储；理论碰撞风险可忽略，
但若内容不可被信任到需要对抗碰撞，可在命中后再比较对象大小或完整内容。

**连接超时为什么不能直接杀 Worker？**

C++ 没有安全的通用杀线程机制，强停可能让锁、事务和对象停在不一致状态。正确做法
是传递 cancellation token、在阶段边界检查、给下游调用设置期限，并把写操作设计成
事务化和可补偿。

**本次仍有哪些生产级缺口？**

密码应使用 Argon2id/bcrypt 而不是普通 SHA-256；大规模部署应把文件迁移到对象存储，
并为隔离目录增加启动巡检/outbox；还应补充 CSRF、防病毒/完整图片解码校验、全链路
trace、按场景隔离的 Worker 池，以及 Redis/MySQL 多副本与故障演练。
