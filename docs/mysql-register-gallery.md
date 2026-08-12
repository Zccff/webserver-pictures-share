# MySQL register/gallery deployment

This server now exposes a simple registration/login UI and gallery page.

## Build dependencies

On a Debian/Ubuntu server:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config default-libmysqlclient-dev
```

If your server uses MariaDB packages, `libmariadb-dev` is also usually compatible
because it provides the `mysqlclient` pkg-config entry.

## Database settings

The program reads MySQL connection settings from environment variables:

```bash
export MYSQL_HOST=127.0.0.1
export MYSQL_PORT=3306
export MYSQL_USER=root
export MYSQL_PASSWORD='your_password'
export MYSQL_DATABASE=my_web_server
```

`MYSQL_DATABASE` is created automatically if the MySQL user has permission.
The server also creates this table automatically:

```sql
CREATE TABLE IF NOT EXISTS users (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE,
    password_hash VARCHAR(64) NOT NULL,
    display_name VARCHAR(64) NOT NULL,
    email VARCHAR(128) NOT NULL DEFAULT '',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

## Build and run

```bash
cmake -S . -B build
cmake --build build
./build/my_web_server 9006 ./webroot
```

Then open:

```text
http://SERVER_IP:9006/
```

You can also use the project startup script. It sets MySQL environment
variables before starting the server:

```bash
bash start.sh
```

Optional arguments:

```bash
bash start.sh 9006 ./webroot 8
```

The defaults in `start.sh` are:

```bash
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=webuser
MYSQL_PASSWORD=123456
MYSQL_DATABASE=my_web_server
```

You can override them without editing the script:

```bash
MYSQL_PASSWORD='your_password' bash start.sh
```

## APIs

- `POST /api/register`
  - form fields: `username`, `password`, `display_name`, `email`
- `POST /api/login`
  - form fields: `username`, `password`
- `GET /api/images`
  - scans `webroot/images` and returns image names and URLs
- `GET /api/db-status`
  - returns whether MySQL support is active

Put `.png`, `.jpg`, `.jpeg`, `.webp`, or `.gif` files in `webroot/images`.
They will appear in the gallery automatically.
