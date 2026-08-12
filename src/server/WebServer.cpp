#include "server/WebServer.h"

#include "log/AsyncLogger.h"
#include "utils/SocketUtil.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

WebServer::WebServer(
    int port,
    std::string web_root,
    size_t worker_count
)
    : port_(port),
    web_root_(std::move(web_root)),
    file_cache_(10 * 1024 * 1024),
    worker_pool_(worker_count) {
}

WebServer::~WebServer() {
    if (listen_fd_ >= 0) {
        SocketUtil::close_fd(listen_fd_);
        listen_fd_ = -1;
    }

    worker_pool_.stop();

    for (auto& sub : sub_reactors_) {
        sub->stop();
    }
}

bool WebServer::init() {
    listen_fd_ = SocketUtil::create_listen_fd(port_);

    if (listen_fd_ < 0) {
        return false;
    }

    try {
        sub_reactors_.reserve(SUB_REACTOR_COUNT);

        for (size_t i = 0; i < SUB_REACTOR_COUNT; ++i) {
            sub_reactors_.push_back(std::make_unique<SubReactor>(
                i,
                web_root_,
                worker_pool_,
                file_cache_
            ));
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("create sub reactors failed, reason=" + std::string(e.what()));
        return false;
    }

    LOG_INFO(
        "server init success, main=1, sub=" +
        std::to_string(SUB_REACTOR_COUNT)
    );
    return true;
}

void WebServer::start() {
    for (auto& sub : sub_reactors_) {
        sub->start();
    }

    std::cout << "MyWebServer Plus started." << std::endl;
    std::cout << "Listening on port: " << port_ << std::endl;
    std::cout << "Web root: " << web_root_ << std::endl;
    std::cout << "Reactors: 1 main + " << SUB_REACTOR_COUNT << " sub" << std::endl;
    std::cout << "Visit: http://127.0.0.1:" << port_ << std::endl;
    std::cout << "Metrics: http://127.0.0.1:" << port_ << "/metrics" << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = accept4(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_addr_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            LOG_ERROR(
                "accept failed, reason=" +
                std::string(std::strerror(errno))
            );

            if (errno == EMFILE || errno == ENFILE ||
                errno == ENOBUFS || errno == ENOMEM) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            break;
        }

        SubReactor& target = *sub_reactors_[next_sub_];
        next_sub_ = (next_sub_ + 1) % sub_reactors_.size();
        target.enqueue_connection(client_fd);

        if (AsyncLogger::instance().should_log("INFO")) {
            char client_ip[INET_ADDRSTRLEN]{};
            inet_ntop(
                AF_INET,
                &client_addr.sin_addr,
                client_ip,
                sizeof(client_ip)
            );

            std::ostringstream oss;
            oss << "new connection "
                << client_ip << ":" << ntohs(client_addr.sin_port)
                << " fd=" << client_fd
                << " sub=" << ((next_sub_ + sub_reactors_.size() - 1) % sub_reactors_.size());
            LOG_INFO(oss.str());
        }
    }
}
