# MyWebServer v5 离线部署

本包面向 Linux x86_64/amd64。只携带 `mywebserver-app:opt-v5` 应用镜像；远程
服务器应已存在以下镜像：

- `mysql:8.4`
- `redis:7-alpine`
- `nginx:1.27-alpine`

## 部署

```bash
cd /opt/v5
tar -xzf MyWebServer-deploy-v5-linux-amd64.tar.gz --strip-components=1
sha256sum -c SHA256SUMS
[ -f .env ] || cp .env.example .env
vi .env
chmod +x deploy.sh
./deploy.sh
```

必须修改 `.env` 中的 `MYSQL_ROOT_PASSWORD` 和 `MYSQL_PASSWORD`。默认公网端口为
8080；如需直接使用 80 端口，将 `PUBLIC_PORT=8080` 改成 `PUBLIC_PORT=80`。

查看状态和日志：

```bash
docker compose -p mywebserver-v5 --env-file .env -f docker-compose.yml ps
docker compose -p mywebserver-v5 --env-file .env -f docker-compose.yml logs -f app nginx
curl http://127.0.0.1:8080/metrics
```

部署脚本从 `.env` 的 `COMPOSE_PROJECT_NAME` 读取项目名，默认是
`mywebserver-v5`。不要执行 `docker compose down -v`，其中 `-v` 会删除数据卷。
升级前仍建议备份数据库和图片卷。

## 从 `/opt/serverv4` 复用数据

旧配置中的 `./webroot` 是公开静态目录。由于 app 的 `/app/webroot/images` 和 Nginx
的 `/usr/share/nginx/protected-images` 都挂载了 `image_data`，实际上传图片通常在 Docker
卷中，而不是宿主机的 `webroot/images`。

先确认正在使用的卷名：

```bash
docker inspect mywebserver-mysql --format '{{range .Mounts}}{{println .Name .Destination}}{{end}}'
docker inspect mywebserver-redis --format '{{range .Mounts}}{{println .Name .Destination}}{{end}}'
docker inspect mywebserver-app --format '{{range .Mounts}}{{println .Name .Destination}}{{end}}'
```

本机确认的实际输出分别是 `mywebserver_mysql_data`、`mywebserver_redis_data` 和
`mywebserver_image_data`。先停掉 v4，但不要加 `-v`：

```bash
cd /opt/serverv4
docker compose -f docker-compose.yml down
```

本项目已经提供 serverv4 迁移模板。复制后填写原有 MySQL 密码：

```bash
cd /opt/v5
cp .env.reuse-serverv4.example .env
vi .env
./deploy.sh
```

卷名必须以 `docker inspect` 的实际输出为准。`external=true` 会在卷名写错时停止部署，
而不是静默创建一个空数据库。不能让 v4 和 v5 两个 MySQL 容器同时挂载同一个数据卷。

## 架构说明

Nginx 是唯一暴露端口的容器。MySQL、Redis 和 app 仅位于 internal backend 网络；
静态首页由 Nginx 发送，受保护图片由 C++ 完成 Session 校验后通过
`X-Accel-Redirect` 交给 Nginx 发送。

上传接口先由 Nginx 内部鉴权并限速，再采用流式落盘；缩略图由有界后台队列异步
生成，单任务默认 15 秒超时，不会把整张图片长期留在业务线程内存中。点击榜和
下载榜分别存入 Redis Sorted Set，并通过
`appendonly yes`、`appendfsync everysec` 持久化到复用的 Redis 数据卷；异常断电时
理论上最多丢失约 1 秒尚未刷盘的热度增量。可通过
`GET /api/rankings?type=views&limit=10` 和
`GET /api/rankings?type=downloads&limit=10` 查询。
删除使用“文件隔离 → Redis token 栅栏 → MySQL 提交 → Redis 派生数据清理”流程；
上传和删除各有代际 token，阻止晚到的上传激活、缩略图状态或热度增量重新写回。
异常中断留下的 `.deleting` 文件会在下次启动时按 MySQL 元数据自动恢复或清除。
图片列表缓存使用版本校验，失效前启动的旧扫描不会污染新缓存。上传请求必须带
`Content-Length`；Nginx 会在入口拒绝 chunked 上传，避免流式请求传到一半才失败。
