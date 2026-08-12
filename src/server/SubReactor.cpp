#include "server/SubReactor.h"

#include "http/HttpResponse.h"
#include "log/AsyncLogger.h"
#include "utils/Metrics.h"
#include "utils/SocketUtil.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

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

    if (thread_.joinable()) {
        thread_.join();
    }
}

void SubReactor::enqueue_connection(int client_fd) {
    if (!running_.load(std::memory_order_acquire)) {
        SocketUtil::close_fd(client_fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_connections_.push(client_fd);
    }

    wake();
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
        return;
    }

    uint64_t generation = next_generation_++;

    if (!epoller_.add_fd_with_data(
        client_fd,
        CLIENT_READ_EVENTS,
        event_token(client_fd, generation)
    )) {
        SocketUtil::close_fd(client_fd);
        return;
    }

    connections_[client_fd] = std::make_unique<HttpConnection>(
        client_fd,
        generation
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

    try {
        worker_pool_.submit([
            this,
            client_fd,
            generation,
            request = std::move(request),
            valid_request,
            keep_alive
        ]() mutable {
            Completion completion;
            completion.fd = client_fd;
            completion.generation = generation;
            completion.keep_alive = keep_alive;

            try {
                if (valid_request && AsyncLogger::instance().should_log("INFO")) {
                    LOG_INFO(
                        "request fd=" + std::to_string(client_fd) +
                        " method=" + request.method() +
                        " path=" + request.path()
                    );
                }

                HttpResponse response_builder(
                    web_root_,
                    file_cache_,
                    worker_pool_.queue_size()
                );

                completion.response = valid_request
                    ? response_builder.build_response(request, keep_alive)
                    : response_builder.build_response("GET", "/404.html", false);

                if (response_builder.has_send_file()) {
                    completion.file_path = response_builder.send_file_path();
                    completion.file_size = response_builder.send_file_size();
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR(
                    "worker response failed, fd=" + std::to_string(client_fd) +
                    ", reason=" + e.what()
                );
                completion.close_only = true;
            }

            enqueue_completion(std::move(completion));
        });
    }
    catch (const std::exception& e) {
        LOG_ERROR(
            "worker submit failed, fd=" + std::to_string(client_fd) +
            ", reason=" + e.what()
        );

        Completion completion;
        completion.fd = client_fd;
        completion.generation = generation;
        completion.close_only = true;
        enqueue_completion(std::move(completion));
    }
}

void SubReactor::enqueue_completion(Completion completion) {
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
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
        CONNECTION_TIMEOUT_MS,
        [this, client_fd, generation]() {
            LOG_WARN("connection timeout, fd=" + std::to_string(client_fd));
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
    epoller_.del_fd(client_fd);
    SocketUtil::close_fd(client_fd);
    connections_.erase(it);
    Metrics::instance().dec_active_connections();
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
        pending.pop();
    }
}

uint64_t SubReactor::event_token(int client_fd, uint64_t generation) {
    return ((generation & 0xffffffffULL) << 32) |
        static_cast<uint32_t>(client_fd);
}
