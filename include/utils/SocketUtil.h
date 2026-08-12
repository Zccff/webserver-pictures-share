#pragma once

namespace SocketUtil {

	int set_nonblocking(int fd);

	int create_listen_fd(int port, int backlog = 128);

	void close_fd(int fd);

}