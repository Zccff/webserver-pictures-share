#include "auth/AsyncSessionManager.h"
#include "redis/AsyncRedisClient.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename Future>
bool ready_within(Future& future, std::chrono::seconds timeout) {
    return future.wait_for(timeout) == std::future_status::ready;
}

}

int main() {
    const char* enabled = std::getenv("RUN_REDIS_INTEGRATION_TESTS");

    if (!enabled || std::string(enabled) != "1") {
        std::cout << "set RUN_REDIS_INTEGRATION_TESTS=1 to run\n";
        return 77;
    }

    AsyncRedisClient& client = AsyncRedisClient::instance();

    if (!client.init_from_env()) {
        std::cerr << client.status_message() << '\n';
        return 8;
    }

    if (!AsyncSessionManager::instance().init_from_env()) {
        std::cerr << AsyncSessionManager::instance().status_message() << '\n';
        return 1;
    }

    std::promise<std::string> created;
    auto created_future = created.get_future();
    AsyncSessionManager::instance().create_session(
        "async_test_user",
        "Async Test",
        [&created](std::string token, std::string error) {
            if (!error.empty()) {
                created.set_value("");
                return;
            }
            created.set_value(std::move(token));
        }
    );

    if (!ready_within(created_future, std::chrono::seconds(5))) {
        std::cerr << "create session timed out\n";
        return 2;
    }

    std::string token = created_future.get();

    if (token.empty()) {
        std::cerr << "create session failed\n";
        return 3;
    }

    std::promise<AsyncRedisResult> redis_key;
    auto redis_key_future = redis_key.get_future();
    client.command(
        { "GET", client.key_prefix() + "session:" + token },
        [&redis_key](AsyncRedisResult result) {
            redis_key.set_value(std::move(result));
        }
    );

    if (!ready_within(redis_key_future, std::chrono::seconds(5)) ||
        redis_key_future.get().type != RedisResultType::String) {
        std::cerr << "session was not stored through the shared Redis client\n";
        return 9;
    }

    std::promise<SessionInfo> fetched;
    auto fetched_future = fetched.get_future();
    AsyncSessionManager::instance().get_session(
        token,
        [&fetched](bool found, SessionInfo session, std::string) {
            fetched.set_value(found ? std::move(session) : SessionInfo{});
        }
    );

    if (!ready_within(fetched_future, std::chrono::seconds(5)) ||
        fetched_future.get().username != "async_test_user") {
        std::cerr << "get session failed\n";
        return 4;
    }

    constexpr int PIPELINE_COMMANDS = 512;
    std::vector<std::future<bool>> pipeline_futures;
    pipeline_futures.reserve(PIPELINE_COMMANDS);

    for (int i = 0; i < PIPELINE_COMMANDS; ++i) {
        auto promise = std::make_shared<std::promise<bool>>();
        pipeline_futures.push_back(promise->get_future());
        AsyncSessionManager::instance().get_session(
            token,
            [promise](bool found, SessionInfo, std::string error) {
                promise->set_value(found && error.empty());
            }
        );
    }

    for (auto& future : pipeline_futures) {
        if (!ready_within(future, std::chrono::seconds(5)) || !future.get()) {
            std::cerr << "pipelined get failed\n";
            return 5;
        }
    }

    std::promise<bool> destroyed;
    auto destroyed_future = destroyed.get_future();
    AsyncSessionManager::instance().destroy_session(
        token,
        [&destroyed](bool ok, std::string) { destroyed.set_value(ok); }
    );

    if (!ready_within(destroyed_future, std::chrono::seconds(5)) ||
        !destroyed_future.get()) {
        std::cerr << "destroy session failed\n";
        return 6;
    }

    std::promise<bool> missing;
    auto missing_future = missing.get_future();
    AsyncSessionManager::instance().get_session(
        token,
        [&missing](bool found, SessionInfo, std::string error) {
            missing.set_value(!found && error.empty());
        }
    );

    if (!ready_within(missing_future, std::chrono::seconds(5)) ||
        !missing_future.get()) {
        std::cerr << "deleted session still exists\n";
        return 7;
    }

    std::cout << "async session manager test passed\n";
    return 0;
}
