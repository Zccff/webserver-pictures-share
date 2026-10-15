FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=http://mirrors.aliyun.com/ubuntu

RUN sed -i "s|http://archive.ubuntu.com/ubuntu|${APT_MIRROR}|g; s|http://security.ubuntu.com/ubuntu|${APT_MIRROR}|g" /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || true

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    default-libmysqlclient-dev \
    libhiredis-dev \
    libuv1-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=http://mirrors.aliyun.com/ubuntu

RUN sed -i "s|http://archive.ubuntu.com/ubuntu|${APT_MIRROR}|g; s|http://security.ubuntu.com/ubuntu|${APT_MIRROR}|g" /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || true

RUN apt-get update && apt-get install -y --no-install-recommends \
    default-libmysqlclient-dev \
    libhiredis-dev \
    libuv1-dev \
    libvips-tools \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/build/my_web_server /app/my_web_server
COPY webroot /app/webroot
COPY webroot /app/webroot-seed
COPY deploy/docker-entrypoint.sh /app/docker-entrypoint.sh

RUN chmod +x /app/docker-entrypoint.sh

ENV MYSQL_HOST=mysql
ENV MYSQL_PORT=3306
ENV MYSQL_USER=webuser
ENV MYSQL_DATABASE=my_web_server
ENV REDIS_HOST=redis
ENV REDIS_PORT=6379
ENV SESSION_TTL_SECONDS=86400
ENV SESSION_CACHE_TTL_SECONDS=3
ENV LOGIN_CACHE_TTL_SECONDS=3
ENV REDIS_POOL_SIZE=8
ENV MYSQL_POOL_SIZE=8
ENV X_ACCEL_REDIRECT=1
ENV MYWEBSERVER_LOG_LEVEL=WARN
ENV IMAGE_LIST_CACHE_TTL_SECONDS=3
ENV IMAGE_STATUS_CACHE_TTL_SECONDS=30
ENV IMAGE_PROCESSOR_THREADS=2
ENV IMAGE_PROCESSOR_QUEUE_CAPACITY=256
ENV THUMBNAIL_MAX_WIDTH=480
ENV THUMBNAIL_MAX_HEIGHT=480
ENV THUMBNAIL_WEBP_QUALITY=82

EXPOSE 9006

ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["/app/my_web_server", "9006", "/app/webroot", "6"]
