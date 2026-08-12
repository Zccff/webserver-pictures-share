#pragma once

#include "cache/LRUCache.h"
#include "http/HttpConnection.h"
#include "server/Epoller.h"
#include "threadpool/ThreadPool.h"
#include "timer/HeapTimer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

class SubReactor {
public:
    SubReactor(
        size_t id,
        std::string web_root,
        ThreadPool& worker_pool,
        LRUCache& file_cache
    );

    ~SubReactor();

    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;

    void start();

    void stop();

    void enqueue_connection(int client_fd);

private:
    struct Completion {
        int fd{ -1 };
        uint64_t generation{ 0 };
        std::string response;
        std::string file_path;
        uint64_t file_size{ 0 };
        bool keep_alive{ false };
        bool close_only{ false };
    };

    static constexpr int CONNECTION_TIMEOUT_MS = 60000;
    static constexpr uint32_t CLIENT_READ_EVENTS =
        EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    static constexpr uint32_t CLIENT_WRITE_EVENTS =
        EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT;

    void run();

    void wake();

    void drain_wakeup_fd();

    void drain_queues();

    void adopt_connection(int client_fd);

    void dispatch_client_event(
        int client_fd,
        uint64_t generation,
        uint32_t events
    );

    void handle_readable(int client_fd);

    void handle_writable(int client_fd);

    void submit_request(
        int client_fd,
        uint64_t generation,
        HttpRequest request,
        bool valid_request
    );

    void enqueue_completion(Completion completion);

    void apply_completion(Completion completion);

    void refresh_timer(int client_fd, uint64_t generation);

    void close_connection(int client_fd, uint64_t generation);

    void close_all_connections();

    static uint64_t event_token(int client_fd, uint64_t generation);

private:
    size_t id_;
    std::string web_root_;
    ThreadPool& worker_pool_;
    LRUCache& file_cache_;

    Epoller epoller_;
    HeapTimer timer_;
    int wake_fd_{ -1 };
    std::thread thread_;
    std::atomic<bool> running_{ false };

    std::mutex pending_mutex_;
    std::queue<int> pending_connections_;

    std::mutex completion_mutex_;
    std::queue<Completion> completions_;

    uint64_t next_generation_{ 1 };
    std::unordered_map<int, std::unique_ptr<HttpConnection>> connections_;
};
