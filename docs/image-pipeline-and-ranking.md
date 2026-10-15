# 图片传输、缩略图与 Redis 热榜

## 上传链路

浏览器不再构造 multipart 副本，而是把 `File` 作为请求体直接发送，并携带：

- `Content-Type: image/*`
- `X-File-Name: <URL 编码文件名>`
- `Idempotency-Key: <每次上传的唯一值>`

Nginx 先用 `auth_request` 对 `/api/me` 发无正文内部鉴权请求；未登录请求不会把大
请求体转发给 app。通过鉴权后，Nginx 对 `/api/upload-image` 关闭
`proxy_request_buffering`。C++ 的 SubReactor 在
ET 模式下持续 `recv()` 到 `EAGAIN`，识别这个直传协议后将正文分块写入
`webroot/images/.upload-tmp`，不会把整张图片保存在 `HttpRequest::body()`。

请求完整后才交给 Worker。Worker 分块计算 SHA-256、检查文件签名与扩展名、在
MySQL 中原子预留元数据，然后用同一文件系统内的 `rename()` 发布原图。旧版
multipart 接口仍保留兼容，但网页默认走直传路径。

上传成功只代表原图已发布。缩略图任务进入独立的有界 `ImageProcessor` 队列，
由固定数量线程调用 `vipsthumbnail` 生成 WebP。状态保存在 Redis：

```text
PROCESSING -> READY
           -> FAILED
```

客户端可查询 `GET /api/image-status?name=<保存后的文件名>`。缩略图存在时列表
返回 `/thumbnails/<name>.webp`；C++ 鉴权后用 `X-Accel-Redirect` 让 Nginx 从
共享图片卷发送，原图与下载也采用相同方式。
`vipsthumbnail` 默认最多执行 15 秒，超时先发送 SIGTERM，再用 SIGKILL 兜底，防止
损坏图片或外部工具异常永久占住图片处理线程。

## 热榜数据结构

两个排行榜使用 Redis Sorted Set：

```text
myweb:v5:image:rank:views       member=图片保存名 score=主动查看次数
myweb:v5:image:rank:downloads   member=图片保存名 score=已授权下载请求次数
```

`ZINCRBY` 原子累加，多个应用实例并发更新不会丢计数。读取使用 `ZREVRANGE
WITHSCORES`，并通过 Lua 一次返回 JSON 数组，避免多次网络往返。接口为：

```text
POST /api/image-view?name=<图片保存名>   # 用户主动打开图片时计数
GET /api/rankings?type=views&limit=20
GET /api/rankings?type=downloads&limit=20
```

`limit` 必须在 1 到 100 之间。接口需要 Session；删除自己拥有的图片后，Lua 脚本在
MySQL 提交后同时从两个 ZSET 移除该 member，并把生命周期推进为 `DELETED`。删除开始
时的 `DELETING` 栅栏会隐藏该图片，并拒绝热度累加、缩略图状态和旧上传激活。重新
上传同一内容必须先取得新的 upload token，再由该 token 从 `UPLOADING` 推进到
`ACTIVE`。缩略图、列表加载和原图的被动 GET 均不计入点击榜；每次用户主动打开图片
都会调用计数接口，即使是同一账号反复点击也分别计数。列表缓存还用版本号阻止旧扫描
结果回填。页面只展示两个全站榜的前 5 名，接口最多可请求前 100 名。

同分 member 的顺序遵循 Redis `ZREVRANGE` 的确定性反向字典序。当前是全站榜；
如果以后做日榜，可以增加 `image:rank:views:YYYYMMDD` 并设置过期时间。

## 持久化与一致性

Compose 中 Redis 使用：

```text
--appendonly yes --appendfsync everysec
```

`redis_data` 命名卷保存 AOF，所以容器重建或主机重启后榜单仍在。`everysec`
通常最多损失约一秒尚未刷盘的热度增量，这是吞吐与持久性的折中。热度不是账户、
权限或图片归属的事实来源，因此不与 MySQL 做双写事务；MySQL 仍负责强一致的用户
和图片所有权。

## 背压边界

- Redis 命令队列、单连接 pipeline、Worker、日志、完成队列和连接分发队列均有上限。
- 图片处理队列满时上传仍可保留原图，但状态标记为 `FAILED`，不会无限堆积任务。
- Redis 排行写入失败不会阻断图片读取或下载，失败会记录日志；API 读取失败返回 503。
- 上传临时文件由 `HttpRequest` 的共享所有权清理；校验失败、限流、断开和异常路径均会删除。
- 原图发布使用原子重命名，避免列表或 Nginx 看到半个文件。
