# Docker Compose deployment

This project can run as four containers:

- `nginx`: public reverse proxy
- `app`: C++ WebServer
- `mysql`: user storage
- `redis`: session storage

## Start

```bash
docker compose up -d --build
```

Open:

```text
http://127.0.0.1:8080/
```

The nginx container maps host port `8080` to container port `80`.
When you have a public server, open firewall/security-group port `8080`,
or change `docker-compose.yml` to map `80:80`.

## Public IP / domain later

For direct public IP access, no code change is required. Point the browser to:

```text
http://PUBLIC_IP:8080/
```

For a domain, edit `deploy/nginx/default.conf`:

```nginx
server_name example.com;
```

Then restart nginx:

```bash
docker compose restart nginx
```

## Data

Persistent volumes:

- `mysql_data`: MySQL data
- `redis_data`: Redis appendonly session data
- `image_data`: uploaded images

Uploaded images are stored in:

```text
/app/webroot/images
```

inside the app container.

The nginx container also mounts the same `image_data` volume read-only at:

```text
/usr/share/nginx/protected-images
```

Image requests still go through the C++ app first for Redis session
validation. After validation, the app returns `X-Accel-Redirect` and nginx
serves the image file internally with `sendfile`.

## Environment

The app connects to services by Docker Compose service names:

```text
MYSQL_HOST=mysql
REDIS_HOST=redis
```

Default credentials in `docker-compose.yml`:

```text
MYSQL_USER=webuser
MYSQL_PASSWORD=123456
MYSQL_DATABASE=my_web_server
```

Change these before exposing the service on a real public machine.

Performance-related defaults:

```text
MYSQL_POOL_SIZE=8
REDIS_POOL_SIZE=8
SESSION_CACHE_TTL_SECONDS=3
X_ACCEL_REDIRECT=1
```

`X_ACCEL_REDIRECT=1` requires the nginx `/protected-images/` internal location
and the shared `image_data` volume mount.

## Useful commands

```bash
docker compose ps
docker compose logs -f app
docker compose logs -f nginx
docker compose down
docker compose down -v
```

`docker compose down -v` deletes MySQL, Redis, and uploaded image volumes.
