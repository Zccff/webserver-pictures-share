#include "redis/AsyncRedisClient.h"

#include "utils/Metrics.h"

#include <hiredis/adapters/libuv.h>
#include <hiredis/async.h>
#include <hiredis/hiredis.h>
#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace {

std::string env_or_default(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

int positive_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

size_t positive_env_size(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
    return parsed > 0 ? parsed : fallback;
}

std::string normalize_prefix(std::string prefix) {
    if (prefix.empty()) {
        prefix = "myweb:v5:";
    }
    if (prefix.back() != ':') {
        prefix.push_back(':');
    }
    return prefix;
}

}

struct AsyncRedisClient::Impl {
    struct Command {
        std::vector<std::string> args;
        Callback callback;
        std::chrono::steady_clock::time_point deadline;
    };

    struct Connection {
        Impl* owner{ nullptr };
        redisAsyncContext* context{ nullptr };
        bool ready{ false };
        bool authenticating{ false };
        size_t outstanding{ 0 };
        std::chrono::steady_clock::time_point reconnect_at{};
    };

    struct CallbackState {
        Impl* owner{ nullptr };
        Connection* connection{ nullptr };
        Callback callback;
    };

    std::string host{ "127.0.0.1" };
    int port{ 6379 };
    std::string password;
    std::string prefix{ "myweb:v5:" };
    size_t pool_size{ 8 };
    int command_timeout_ms{ 3000 };
    size_t command_capacity{ 4096 };
    size_t max_pipeline_per_connection{ 256 };

    mutable std::mutex state_mutex;
    bool available{ false };
    std::string status{ "Redis async client is not initialized" };

    mutable std::mutex command_mutex;
    mutable std::deque<std::unique_ptr<Command>> pending_commands;
    mutable std::atomic<size_t> command_count{ 0 };

    uv_loop_t loop{};
    uv_async_t wake_handle{};
    uv_timer_t maintenance_timer{};
    std::vector<std::unique_ptr<Connection>> connections;
    std::thread loop_thread;
    std::atomic<bool> running{ false };
    bool shutdown_started{ false };

    std::mutex startup_mutex;
    std::condition_variable startup_cv;
    bool loop_ready{ false };

    ~Impl() {
        stop();
    }

    void configure_from_env() {
        host = env_or_default("REDIS_HOST", "127.0.0.1");
        port = positive_env_int("REDIS_PORT", 6379);
        password = env_or_default("REDIS_PASSWORD", "");
        prefix = normalize_prefix(env_or_default("REDIS_KEY_PREFIX", "myweb:v5:"));
        pool_size = positive_env_size("REDIS_POOL_SIZE", 8);
        command_timeout_ms = positive_env_int("REDIS_IO_TIMEOUT_MS", 3000);
        command_capacity = positive_env_size("REDIS_COMMAND_CAPACITY", 4096);
        max_pipeline_per_connection = positive_env_size(
            "REDIS_MAX_PIPELINE_PER_CONNECTION",
            256
        );
    }

    bool start() {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) {
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(startup_mutex);
            loop_ready = false;
        }
        shutdown_started = false;
        loop_thread = std::thread([this]() { loop_main(); });
        std::unique_lock<std::mutex> lock(startup_mutex);
        startup_cv.wait(lock, [this]() { return loop_ready; });
        return running.load(std::memory_order_acquire);
    }

    void stop() {
        if (!running.exchange(false)) {
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            return;
        }

        uv_async_send(&wake_handle);
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
        set_status(false, "Redis async client is stopped");
    }

    void loop_main() {
        if (uv_loop_init(&loop) != 0) {
            startup_failed("initialize Redis libuv loop failed");
            return;
        }

        wake_handle.data = this;
        maintenance_timer.data = this;

        if (uv_async_init(&loop, &wake_handle, &Impl::on_wakeup) != 0) {
            uv_loop_close(&loop);
            startup_failed("initialize Redis wake handle failed");
            return;
        }

        if (uv_timer_init(&loop, &maintenance_timer) != 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&wake_handle), nullptr);
            uv_run(&loop, UV_RUN_DEFAULT);
            uv_loop_close(&loop);
            startup_failed("initialize Redis maintenance timer failed");
            return;
        }

        connections.clear();
        connections.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            auto connection = std::make_unique<Connection>();
            connection->owner = this;
            connections.push_back(std::move(connection));
        }

        uv_timer_start(&maintenance_timer, &Impl::on_maintenance, 100, 100);
        for (auto& connection : connections) {
            connect(*connection);
        }

        {
            std::lock_guard<std::mutex> lock(startup_mutex);
            loop_ready = true;
        }
        startup_cv.notify_all();

        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        connections.clear();
    }

    void startup_failed(const std::string& message) {
        set_status(false, message);
        running.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(startup_mutex);
            loop_ready = true;
        }
        startup_cv.notify_all();
    }

    static void on_wakeup(uv_async_t* handle) {
        auto* self = static_cast<Impl*>(handle->data);
        if (!self->running.load(std::memory_order_acquire)) {
            self->shutdown_loop();
            return;
        }
        self->dispatch_commands();
    }

    static void on_maintenance(uv_timer_t* handle) {
        auto* self = static_cast<Impl*>(handle->data);
        if (!self->running.load(std::memory_order_acquire)) {
            self->shutdown_loop();
            return;
        }

        auto now = std::chrono::steady_clock::now();
        for (auto& connection : self->connections) {
            if (!connection->context && now >= connection->reconnect_at) {
                self->connect(*connection);
            }
        }
        self->expire_queued_commands(now);
        self->dispatch_commands();
    }

    void shutdown_loop() {
        if (shutdown_started) {
            return;
        }
        shutdown_started = true;
        fail_all_queued("Redis async client is stopping");
        uv_timer_stop(&maintenance_timer);

        for (auto& connection : connections) {
            connection->ready = false;
            if (connection->context) {
                redisAsyncFree(connection->context);
                connection->context = nullptr;
            }
        }

        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&maintenance_timer))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&maintenance_timer), nullptr);
        }
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&wake_handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&wake_handle), nullptr);
        }
    }

    void connect(Connection& connection) {
        if (!running.load(std::memory_order_acquire) || connection.context) {
            return;
        }

        redisOptions options{};
        REDIS_OPTIONS_SET_TCP(&options, host.c_str(), port);
        timeval timeout{};
        timeout.tv_sec = command_timeout_ms / 1000;
        timeout.tv_usec = (command_timeout_ms % 1000) * 1000;
        options.connect_timeout = &timeout;
        options.command_timeout = &timeout;

        redisAsyncContext* context = redisAsyncConnectWithOptions(&options);
        if (!context || context->err) {
            std::string error = context ? context->errstr : "out of memory";
            if (context) {
                redisAsyncFree(context);
            }
            connection.reconnect_at = std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            set_status(false, "Redis async connect failed: " + error);
            return;
        }

        connection.context = context;
        connection.ready = false;
        context->data = &connection;
        if (redisLibuvAttach(context, &loop) != REDIS_OK ||
            redisAsyncSetConnectCallback(context, &Impl::on_connect) != REDIS_OK ||
            redisAsyncSetDisconnectCallback(context, &Impl::on_disconnect) != REDIS_OK ||
            redisAsyncSetTimeout(context, timeout) != REDIS_OK) {
            connection.context = nullptr;
            redisAsyncFree(context);
            connection.reconnect_at = std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            set_status(false, "attach Redis connection to libuv failed");
        }
    }

    static void on_connect(const redisAsyncContext* context, int status_code) {
        auto* connection = static_cast<Connection*>(context->data);
        if (!connection || !connection->owner) {
            return;
        }
        Impl& self = *connection->owner;

        if (status_code != REDIS_OK) {
            connection->ready = false;
            connection->reconnect_at = std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            self.set_status(false, "Redis async connection failed: " +
                std::string(context->errstr));
            return;
        }

        if (!self.password.empty()) {
            connection->authenticating = true;
            int queued = redisAsyncCommand(
                connection->context,
                &Impl::on_auth,
                connection,
                "AUTH %b",
                self.password.data(),
                self.password.size()
            );
            if (queued != REDIS_OK) {
                connection->authenticating = false;
                redisAsyncDisconnect(connection->context);
            }
            return;
        }

        connection->ready = true;
        self.refresh_status();
        self.dispatch_commands();
    }

    static void on_auth(redisAsyncContext*, void* raw_reply, void* private_data) {
        auto* connection = static_cast<Connection*>(private_data);
        if (!connection || !connection->owner) {
            return;
        }
        connection->authenticating = false;
        auto* reply = static_cast<redisReply*>(raw_reply);
        bool authenticated = reply && reply->type == REDIS_REPLY_STATUS &&
            reply->str && std::string(reply->str, reply->len) == "OK";
        if (!authenticated) {
            connection->owner->set_status(false, "Redis authentication failed");
            redisAsyncDisconnect(connection->context);
            return;
        }
        connection->ready = true;
        connection->owner->refresh_status();
        connection->owner->dispatch_commands();
    }

    static void on_disconnect(const redisAsyncContext* context, int status_code) {
        auto* connection = static_cast<Connection*>(context->data);
        if (!connection || !connection->owner) {
            return;
        }
        Impl& self = *connection->owner;
        connection->context = nullptr;
        connection->ready = false;
        connection->authenticating = false;
        connection->reconnect_at = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        if (status_code != REDIS_OK && self.running.load(std::memory_order_acquire)) {
            self.set_status(false, "Redis async connection lost: " +
                std::string(context->errstr));
        }
        self.refresh_status();
    }

    bool submit(std::vector<std::string> args, Callback callback) const {
        if (!callback) {
            return false;
        }
        if (args.empty()) {
            callback({ RedisResultType::Error, "", 0, "Redis command is empty" });
            return false;
        }
        if (!running.load(std::memory_order_acquire)) {
            callback({ RedisResultType::Error, "", 0, "Redis async client is not running" });
            return false;
        }

        size_t previous = command_count.fetch_add(1, std::memory_order_acq_rel);
        if (previous >= command_capacity) {
            command_count.fetch_sub(1, std::memory_order_acq_rel);
            Metrics::instance().inc_rejected_redis_command();
            callback({ RedisResultType::Error, "", 0, "Redis async command queue is full" });
            return false;
        }

        auto item = std::make_unique<Command>();
        item->args = std::move(args);
        item->callback = std::move(callback);
        item->deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(command_timeout_ms);
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            pending_commands.push_back(std::move(item));
        }
        uv_async_send(const_cast<uv_async_t*>(&wake_handle));
        return true;
    }

    Connection* select_connection() {
        Connection* best = nullptr;
        size_t smallest = std::numeric_limits<size_t>::max();
        for (auto& connection : connections) {
            if (!connection->ready || !connection->context ||
                connection->outstanding >= max_pipeline_per_connection) {
                continue;
            }
            if (connection->outstanding < smallest) {
                best = connection.get();
                smallest = connection->outstanding;
            }
        }
        return best;
    }

    void dispatch_commands() {
        while (running.load(std::memory_order_acquire)) {
            Connection* connection = select_connection();
            if (!connection) {
                return;
            }

            std::unique_ptr<Command> item;
            {
                std::lock_guard<std::mutex> lock(command_mutex);
                if (pending_commands.empty()) {
                    return;
                }
                item = std::move(pending_commands.front());
                pending_commands.pop_front();
            }

            if (std::chrono::steady_clock::now() >= item->deadline) {
                command_count.fetch_sub(1, std::memory_order_acq_rel);
                item->callback({ RedisResultType::Error, "", 0,
                    "Redis command timed out in queue" });
                continue;
            }

            std::vector<const char*> argv;
            std::vector<size_t> lengths;
            argv.reserve(item->args.size());
            lengths.reserve(item->args.size());
            for (const std::string& arg : item->args) {
                argv.push_back(arg.data());
                lengths.push_back(arg.size());
            }

            auto* state = new CallbackState{
                this,
                connection,
                std::move(item->callback)
            };
            ++connection->outstanding;
            int queued = redisAsyncCommandArgv(
                connection->context,
                &Impl::on_reply,
                state,
                static_cast<int>(argv.size()),
                argv.data(),
                lengths.data()
            );
            if (queued != REDIS_OK) {
                --connection->outstanding;
                command_count.fetch_sub(1, std::memory_order_acq_rel);
                Callback failed = std::move(state->callback);
                delete state;
                failed({ RedisResultType::Error, "", 0,
                    "queue Redis command failed" });
            }
        }
    }

    static void on_reply(redisAsyncContext*, void* raw_reply, void* private_data) {
        std::unique_ptr<CallbackState> state(
            static_cast<CallbackState*>(private_data)
        );
        if (!state || !state->owner) {
            return;
        }

        if (state->connection && state->connection->outstanding > 0) {
            --state->connection->outstanding;
        }
        state->owner->command_count.fetch_sub(1, std::memory_order_acq_rel);

        AsyncRedisResult result;
        auto* reply = static_cast<redisReply*>(raw_reply);
        if (!reply) {
            result.error = "Redis connection closed before reply";
        }
        else if (reply->type == REDIS_REPLY_STATUS) {
            result.type = RedisResultType::Status;
            if (reply->str) {
                result.value.assign(reply->str, reply->len);
            }
        }
        else if (reply->type == REDIS_REPLY_STRING) {
            result.type = RedisResultType::String;
            if (reply->str) {
                result.value.assign(reply->str, reply->len);
            }
        }
        else if (reply->type == REDIS_REPLY_INTEGER) {
            result.type = RedisResultType::Integer;
            result.integer = reply->integer;
        }
        else if (reply->type == REDIS_REPLY_NIL) {
            result.type = RedisResultType::Nil;
        }
        else {
            result.type = RedisResultType::Error;
            if (reply->str) {
                result.error.assign(reply->str, reply->len);
            }
            if (result.error.empty()) {
                result.error = "Redis returned an unsupported reply";
            }
        }

        state->callback(std::move(result));
        if (state->owner->running.load(std::memory_order_acquire)) {
            state->owner->dispatch_commands();
        }
    }

    void expire_queued_commands(std::chrono::steady_clock::time_point now) {
        std::deque<std::unique_ptr<Command>> expired;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            while (!pending_commands.empty() &&
                now >= pending_commands.front()->deadline) {
                expired.push_back(std::move(pending_commands.front()));
                pending_commands.pop_front();
            }
        }
        for (auto& item : expired) {
            command_count.fetch_sub(1, std::memory_order_acq_rel);
            item->callback({ RedisResultType::Error, "", 0,
                "Redis command timed out in queue" });
        }
    }

    void fail_all_queued(const std::string& error) {
        std::deque<std::unique_ptr<Command>> queued;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            queued.swap(pending_commands);
        }
        for (auto& item : queued) {
            command_count.fetch_sub(1, std::memory_order_acq_rel);
            item->callback({ RedisResultType::Error, "", 0, error });
        }
    }

    void refresh_status() {
        size_t ready = 0;
        for (const auto& connection : connections) {
            if (connection->ready) {
                ++ready;
            }
        }
        std::ostringstream message;
        message << "Redis async pool ready=" << ready << "/" << pool_size
                << ", in_flight_or_queued="
                << command_count.load(std::memory_order_relaxed);
        set_status(ready > 0, message.str());
    }

    void set_status(bool value, const std::string& message) {
        std::lock_guard<std::mutex> lock(state_mutex);
        available = value;
        status = message;
    }
};

AsyncRedisClient& AsyncRedisClient::instance() {
    static AsyncRedisClient client;
    return client;
}

AsyncRedisClient::AsyncRedisClient() : impl_(std::make_unique<Impl>()) {
}

AsyncRedisClient::~AsyncRedisClient() = default;

bool AsyncRedisClient::init_from_env() {
    if (impl_->running.load(std::memory_order_acquire)) {
        return true;
    }
    impl_->configure_from_env();
    impl_->set_status(false, "Redis async pool is connecting");
    return impl_->start();
}

void AsyncRedisClient::shutdown() {
    impl_->stop();
}

bool AsyncRedisClient::available() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->available;
}

std::string AsyncRedisClient::status_message() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->status;
}

std::string AsyncRedisClient::key_prefix() const {
    return impl_->prefix;
}

bool AsyncRedisClient::command(
    std::vector<std::string> args,
    Callback callback
) const {
    return impl_->submit(std::move(args), std::move(callback));
}
