#include "server/SubReactor.h"

#include "http/HttpResponse.h"
#include "log/AsyncLogger.h"
#include "utils/Metrics.h"
#include "utils/SocketUtil.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {

size_t env_capacity(const char* name, size_t fallback) {
    const char* value = std::getenv(name);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
    return parsed == 0 ? fallback : parsed;
}

int env_timeout(const char* name, int fallback) {
    const char* value = std::getenv(name);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 && parsed <= 3600000L
        ? static_cast<int>(parsed)
        : fallback;
}

}

SubReactor::SubReactor(
    size_t id,
    std::string web_root,
    ThreadPool& worker_pool,
    LRUCache& file_cache
)
    : id_(id),
    web_root_(std::move(web_root)),
    worker_pool_(worker_pool),
    file_cache_(file_cache),
    epoller_(4096) {
    max_connections_ = env_capacity("MAX_CONNECTIONS_PER_SUB_REACTOR", 4096);
    max_pending_connections_ = env_capacity("PENDING_CONNECTION_QUEUE_CAPACITY", 1024);
    max_completions_ = env_capacity("COMPLETION_QUEUE_CAPACITY", 4096);
    connection_timeout_ms_ = env_timeout(
        "CONNECTION_IDLE_TIMEOUT_MS",
        DEFAULT_CONNECTION_TIMEOUT_MS
    );
    business_timeout_ms_ = env_timeout(
        "BUSINESS_TIMEOUT_MS",
        DEFAULT_BUSINESS_TIMEOUT_MS
    );
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (wake_fd_ < 0) {
        throw std::runtime_error(
            "eventfd failed: " + std::string(std::strerror(errno))
        );
    }

    if (!epoller_.add_fd(wake_fd_, EPOLLIN)) {
        close(wake_fd_);
        wake_fd_ = -1;
        throw std::runtime_error("add eventfd to epoll failed");
    }
}

SubReactor::~SubReactor() {
    stop();

    if (wake_fd_ >= 0) {
        close(wake_fd_);
    }
}

void SubReactor::start() {
    bool expected = false;

    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    thread_ = std::thread(&SubReactor::run, this);
}

void SubReactor::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    wake();
    completion_not_full_cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SubReactor::enqueue_connection(int client_fd) {
    if (!running_.load(std::memory_order_acquire)) {
        SocketUtil::close_fd(client_fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        if (pending_connections_.size() >= max_pending_connections_ ||
            owned_connection_count_.load(std::memory_order_relaxed) >= max_connections_) {
            Metrics::instance().inc_rejected_connection();
            SocketUtil::close_fd(client_fd);
            return false;
        }

        owned_connection_count_.fetch_add(1, std::memory_order_relaxed);
        pending_connections_.push(client_fd);
    }

    wake();
    return true;
}

void SubReactor::run() {
    LOG_INFO("sub reactor started, id=" + std::to_string(id_));

    while (running_.load(std::memory_order_acquire)) {
        int event_count = epoller_.wait(timer_.get_next_timeout_ms());

        if (event_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            LOG_ERROR(
                "sub epoll_wait failed, id=" + std::to_string(id_) +
                ", reason=" + std::strerror(errno)
            );
            break;
        }

        for (int i = 0; i < event_count; ++i) {
            uint64_t data = epoller_.get_event_data(i);
            uint32_t events = epoller_.get_events(i);

            if (data == static_cast<uint64_t>(wake_fd_)) {
                drain_wakeup_fd();
                drain_queues();
            }
            else {
                int fd = static_cast<int>(static_cast<uint32_t>(data));
                uint64_t generation = data >> 32;
                dispatch_client_event(fd, generation, events);
            }
        }

        timer_.tick();
    }

    drain_queues();
    close_all_connections();
    LOG_INFO("sub reactor stopped, id=" + std::to_string(id_));
}

void SubReactor::wake() {
    uint64_t one = 1;

    while (write(wake_fd_, &one, sizeof(one)) < 0) {
        if (errno == EINTR) {
            continue;
        }

        break;
    }
}

void SubReactor::drain_wakeup_fd() {
    uint64_t value = 0;

    while (read(wake_fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}

void SubReactor::drain_queues() {
    std::queue<int> pending;
    std::queue<Completion> completed;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        std::swap(pending, pending_connections_);
    }

    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        std::swap(completed, completions_);
    }

    completion_not_full_cv_.notify_all();

    while (!pending.empty()) {
        adopt_connection(pending.front());
        pending.pop();
    }

    while (!completed.empty()) {
        apply_completion(std::move(completed.front()));
        completed.pop();
    }
}

void SubReactor::adopt_connection(int client_fd) {
    if (!running_.load(std::memory_order_acquire)) {
        SocketUtil::close_fd(client_fd);
        owned_connection_count_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    uint64_t generation = next_generation_++;

    if (!epoller_.add_fd_with_data(
        client_fd,
        CLIENT_READ_EVENTS,
        event_token(client_fd, generation)
    )) {
        SocketUtil::close_fd(client_fd);
        owned_connection_count_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    connections_[client_fd] = std::make_unique<HttpConnection>(
        client_fd,
        generation,
        (std::filesystem::path(web_root_) / "images" /
         ".upload-tmp").string()
    );

    Metrics::instance().inc_active_connections();
    Metrics::instance().inc_total_connections();
    refresh_timer(client_fd, generation);
}

void SubReactor::dispatch_client_event(
    int client_fd,
    uint64_t generation,
    uint32_t events
) {
    auto it = connections_.find(client_fd);

    if (it == connections_.end() ||
        it->second->generation() != generation) {
        return;
    }

    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        close_connection(client_fd, generation);
        return;
    }

    if (events & EPOLLOUT) {
        handle_writable(client_fd);
        return;
    }

    if (events & EPOLLIN) {
        handle_readable(client_fd);
    }
}

void SubReactor::handle_readable(int client_fd) {
    auto it = connections_.find(client_fd);

    if (it == connections_.end()) {
        return;
    }

    HttpConnection& connection = *it->second;
    uint64_t generation = connection.generation();
    HttpRequest request;
    HttpConnection::ReadStatus status = connection.read_request(request);

    if (status == HttpConnection::ReadStatus::RequestReady) {
        Metrics::instance().inc_total_requests();
        submit_request(client_fd, generation, std::move(request), true);
        return;
    }

    if (status == HttpConnection::ReadStatus::BadRequest) {
        submit_request(client_fd, generation, HttpRequest{}, false);
        return;
    }

    if (status == HttpConnection::ReadStatus::Incomplete) {
        refresh_timer(client_fd, generation);

        if (!epoller_.mod_fd_with_data(
            client_fd,
            CLIENT_READ_EVENTS,
            event_token(client_fd, generation)
        )) {
            close_connection(client_fd, generation);
        }

        return;
    }

    close_connection(client_fd, generation);
}

void SubReactor::handle_writable(int client_fd) {
    auto it = connections_.find(client_fd);

    if (it == connections_.end()) {
        return;
    }

    HttpConnection& connection = *it->second;
    uint64_t generation = connection.generation();
    HttpConnection::WriteStatus status = connection.flush_response();

    if (status == HttpConnection::WriteStatus::WouldBlock) {
        if (!epoller_.mod_fd_with_data(
            client_fd,
            CLIENT_WRITE_EVENTS,
            event_token(client_fd, generation)
        )) {
            close_connection(client_fd, generation);
        }
        return;
    }

    if (status == HttpConnection::WriteStatus::Error) {
        close_connection(client_fd, generation);
        return;
    }

    if (!connection.keep_alive_after_response()) {
        close_connection(client_fd, generation);
        return;
    }

    refresh_timer(client_fd, generation);
    handle_readable(client_fd);
}

void SubReactor::submit_request(
    int client_fd,
    uint64_t generation,
    HttpRequest request,
    bool valid_request
) {
    bool keep_alive = valid_request && request.is_keep_alive();
    auto it = connections_.find(client_fd);

    if (it == connections_.end() || it->second->generation() != generation) {
        return;
    }

    arm_business_timer(client_fd, generation);
    std::shared_ptr<std::atomic<bool>> cancelled = it->second->cancellation_token();

    bool accepted = worker_pool_.try_submit([
            this,
            client_fd,
            generation,
            cancelled,
            request = std::move(request),
            valid_request,
            keep_alive
        ]() mutable {
            if (cancelled->load(std::memory_order_acquire)) {
                return;
            }

            try {
                if (valid_request && AsyncLogger::instance().should_log("INFO")) {
                    LOG_INFO(
                        "request fd=" + std::to_string(client_fd) +
                        " method=" + request.method() +
                        " path=" + request.path()
                    );
                }

                auto response_builder = std::make_shared<HttpResponse>(
                    web_root_,
                    file_cache_,
                    worker_pool_.queue_size(),
                    cancelled
                );

                HttpResponse::CompletionCallback finish = [
                    this,
                    client_fd,
                    generation,
                    cancelled,
                    keep_alive,
                    response_builder
                ](std::string response) mutable {
                    if (cancelled->load(std::memory_order_acquire)) {
                        return;
                    }

                    Completion completion;
                    completion.fd = client_fd;
                    completion.generation = generation;
                    completion.keep_alive = keep_alive;
                    completion.response = std::move(response);

                    if (response_builder->has_send_file()) {
                        completion.file_path = response_builder->send_file_path();
                        completion.file_size = response_builder->send_file_size();
                    }

                    enqueue_completion(std::move(completion));
                };

                if (!valid_request) {
                    finish(bad_request_response());
                    return;
                }

                HttpResponse::Executor executor = [
                    this,
                    cancelled,
                    finish,
                    keep_alive
                ](std::function<void()> continuation) mutable {
                    if (cancelled->load(std::memory_order_acquire)) {
                        return false;
                    }

                    return worker_pool_.try_submit([
                        cancelled,
                        finish,
                        keep_alive,
                        continuation = std::move(continuation)
                    ]() mutable {
                        if (cancelled->load(std::memory_order_acquire)) {
                            return;
                        }

                        try {
                            continuation();
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("async response continuation failed: " +
                                std::string(e.what()));
                            finish(SubReactor::overload_response(keep_alive));
                        }
                    });
                };

                response_builder->build_response_async(
                    request,
                    keep_alive,
                    std::move(finish),
                    std::move(executor)
                );
            }
            catch (const std::exception& e) {
                LOG_ERROR(
                    "worker response failed, fd=" + std::to_string(client_fd) +
                    ", reason=" + e.what()
                );
                Completion completion;
                completion.fd = client_fd;
                completion.generation = generation;
                completion.close_only = true;
                enqueue_completion(std::move(completion));
            }
        });

    if (!accepted) {
        LOG_WARN("worker queue full, fd=" + std::to_string(client_fd));
        Metrics::instance().inc_rejected_worker_task();

        Completion completion;
        completion.fd = client_fd;
        completion.generation = generation;
        completion.keep_alive = false;
        completion.response = overload_response(false);
        // We already run on the owning SubReactor. Waiting on our own bounded
        // completion queue here would deadlock because only this thread drains it.
        apply_completion(std::move(completion));
    }
}

void SubReactor::enqueue_completion(Completion completion) {
    {
        std::unique_lock<std::mutex> lock(completion_mutex_);
        completion_not_full_cv_.wait(lock, [this]() {
            return !running_.load(std::memory_order_acquire) ||
                completions_.size() < max_completions_;
        });

        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        completions_.push(std::move(completion));
    }

    wake();
}

void SubReactor::apply_completion(Completion completion) {
    auto it = connections_.find(completion.fd);

    if (it == connections_.end() ||
        it->second->generation() != completion.generation) {
        return;
    }

    if (completion.close_only) {
        close_connection(completion.fd, completion.generation);
        return;
    }

    HttpConnection& connection = *it->second;
    connection.begin_response(
        std::move(completion.response),
        std::move(completion.file_path),
        completion.file_size,
        completion.keep_alive
    );

    HttpConnection::WriteStatus status = connection.flush_response();

    if (status == HttpConnection::WriteStatus::WouldBlock) {
        if (!epoller_.mod_fd_with_data(
            completion.fd,
            CLIENT_WRITE_EVENTS,
            event_token(completion.fd, completion.generation)
        )) {
            close_connection(completion.fd, completion.generation);
        }
        return;
    }

    if (status == HttpConnection::WriteStatus::Error || !completion.keep_alive) {
        close_connection(completion.fd, completion.generation);
        return;
    }

    refresh_timer(completion.fd, completion.generation);
    handle_readable(completion.fd);
}

void SubReactor::refresh_timer(int client_fd, uint64_t generation) {
    timer_.add_or_adjust(
        client_fd,
        connection_timeout_ms_,
        [this, client_fd, generation]() {
            LOG_WARN("connection timeout, fd=" + std::to_string(client_fd));
            close_connection(client_fd, generation);
        }
    );
}

void SubReactor::arm_business_timer(int client_fd, uint64_t generation) {
    timer_.add_or_adjust(
        client_fd,
        business_timeout_ms_,
        [this, client_fd, generation]() {
            LOG_WARN("business timeout, fd=" + std::to_string(client_fd));
            close_connection(client_fd, generation);
        }
    );
}

void SubReactor::close_connection(int client_fd, uint64_t generation) {
    auto it = connections_.find(client_fd);

    if (it == connections_.end() || it->second->generation() != generation) {
        return;
    }

    timer_.remove(client_fd);
    it->second->cancel_work();
    epoller_.del_fd(client_fd);
    SocketUtil::close_fd(client_fd);
    connections_.erase(it);
    Metrics::instance().dec_active_connections();
    owned_connection_count_.fetch_sub(1, std::memory_order_relaxed);
    LOG_INFO("close connection, fd=" + std::to_string(client_fd));
}

void SubReactor::close_all_connections() {
    while (!connections_.empty()) {
        auto it = connections_.begin();
        close_connection(it->first, it->second->generation());
    }

    std::queue<int> pending;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        std::swap(pending, pending_connections_);
    }

    while (!pending.empty()) {
        SocketUtil::close_fd(pending.front());
        owned_connection_count_.fetch_sub(1, std::memory_order_relaxed);
        pending.pop();
    }
}

std::string SubReactor::overload_response(bool keep_alive) {
    const std::string body =
        "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n";
    return "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Retry-After: 1\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: " + std::string(keep_alive ? "keep-alive" : "close") +
        "\r\n\r\n" + body;
}

std::string SubReactor::bad_request_response() {
    const std::string body =
        "{ \"ok\": false, \"message\": \"malformed HTTP request\" }\n";
    return "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
}

uint64_t SubReactor::event_token(int client_fd, uint64_t generation) {
    return ((generation & 0xffffffffULL) << 32) |
        static_cast<uint32_t>(client_fd);
}
