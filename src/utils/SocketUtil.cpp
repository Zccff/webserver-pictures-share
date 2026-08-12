#include "utils/SocketUtil.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace SocketUtil {

    int set_nonblocking(int fd) {
        int old_flags = fcntl(fd, F_GETFL);

        if (old_flags < 0) {
            std::cerr << "fcntl F_GETFL failed, fd = " << fd
                << ", errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            return -1;
        }

        int new_flags = old_flags | O_NONBLOCK;

        if (fcntl(fd, F_SETFL, new_flags) < 0) {
            std::cerr << "fcntl F_SETFL failed, fd = " << fd
                << ", errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            return -1;
        }

        return old_flags;
    }

    int create_listen_fd(int port, int backlog) {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

        if (listen_fd < 0) {
            std::cerr << "socket failed, errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            return -1;
        }

        int opt = 1;

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "setsockopt SO_REUSEADDR failed, errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            close(listen_fd);
            return -1;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port);

        if (bind(
            listen_fd,
            reinterpret_cast<sockaddr*>(&server_addr),
            sizeof(server_addr)
        ) < 0) {
            std::cerr << "bind failed, port = " << port
                << ", errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            close(listen_fd);
            return -1;
        }

        if (listen(listen_fd, backlog) < 0) {
            std::cerr << "listen failed, errno = " << errno
                << ", reason = " << std::strerror(errno)
                << std::endl;
            close(listen_fd);
            return -1;
        }

        return listen_fd;
    }

    void close_fd(int fd) {
        if (fd >= 0) {
            close(fd);
        }
    }

}