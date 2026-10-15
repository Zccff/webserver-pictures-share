#include "http/HttpConnection.h"
#include "server/ClientEventPolicy.h"

#include <cerrno>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool write_all(int fd, const std::string& value) {
    size_t offset = 0;
    while (offset < value.size()) {
        ssize_t written = send(
            fd,
            value.data() + offset,
            value.size() - offset,
            MSG_NOSIGNAL
        );
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool expect_bad_request(const std::string& wire_request) {
    int sockets[2]{ -1, -1 };
    if (socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            sockets) != 0) {
        return false;
    }
    bool ok = write_all(sockets[1], wire_request);
    HttpConnection connection(sockets[0], 17);
    HttpRequest request;
    ok = ok && connection.read_request(request) ==
        HttpConnection::ReadStatus::BadRequest;
    close(sockets[0]);
    close(sockets[1]);
    return ok;
}

}

int main() {
    if ((ClientEventPolicy::READ_EVENTS & EPOLLET) == 0 ||
        (ClientEventPolicy::WRITE_EVENTS & EPOLLET) == 0 ||
        (ClientEventPolicy::READ_EVENTS & EPOLLONESHOT) == 0 ||
        (ClientEventPolicy::WRITE_EVENTS & EPOLLONESHOT) == 0) {
        std::cerr << "client event policy must combine ET and ONESHOT\n";
        return 1;
    }

    int sockets[2]{ -1, -1 };
    if (socketpair(
        AF_UNIX,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0,
        sockets
    ) != 0) {
        std::cerr << "socketpair failed\n";
        return 1;
    }

    const std::string pipelined =
        "GET /first HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: test\r\n\r\n";
    if (!write_all(sockets[1], pipelined)) {
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    HttpConnection connection(sockets[0], 9);
    HttpRequest first;
    HttpRequest second;
    if (connection.read_request(first) !=
            HttpConnection::ReadStatus::RequestReady ||
        first.path() != "/first" ||
        connection.read_request(second) !=
            HttpConnection::ReadStatus::RequestReady ||
        second.path() != "/second") {
        std::cerr << "ET read did not drain and retain pipelined requests\n";
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    if (!expect_bad_request(
            "POST /api/upload-image HTTP/1.1\r\n"
            "Host: test\r\n"
            "Content-Length: 4\r\n"
            "Content-Length: 8\r\n\r\nabcdefgh")) {
        std::cerr << "duplicate Content-Length was not rejected\n";
        close(sockets[0]);
        close(sockets[1]);
        return 2;
    }

    if (!expect_bad_request(
            "POST /api/upload-image HTTP/1.1\r\n"
            "Host: test\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "4\r\ntest\r\n0\r\n\r\n")) {
        std::cerr << "unsupported Transfer-Encoding was not rejected\n";
        close(sockets[0]);
        close(sockets[1]);
        return 3;
    }

    if (!expect_bad_request(
            "POST /api/upload-image HTTP/1.1\r\n"
            "Host: test\r\n"
            "Content-Type: image/png\r\n"
            "X-File-Name: test.png\r\n"
            "Content-Length: 4\r\n\r\ntest")) {
        std::cerr << "direct upload without a session cookie was not rejected\n";
        close(sockets[0]);
        close(sockets[1]);
        return 4;
    }

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
