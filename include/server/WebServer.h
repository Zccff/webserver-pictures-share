#pragma once

#include "cache/LRUCache.h"
#include "server/SubReactor.h"
#include "threadpool/ThreadPool.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class WebServer {
public:
    WebServer(
        int port,
        std::string web_root,
        size_t worker_count = 6
    );

    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool init();

    void start();

private:
    static constexpr size_t SUB_REACTOR_COUNT = 2;

    int port_;
    int listen_fd_{ -1 };
    std::string web_root_;

    LRUCache file_cache_;
    ThreadPool worker_pool_;
    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    size_t next_sub_{ 0 };
};
