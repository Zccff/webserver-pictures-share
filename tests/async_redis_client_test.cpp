#include "redis/AsyncRedisClient.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using ResultFuture = std::future<AsyncRedisResult>;

ResultFuture command(AsyncRedisClient& client, std::vector<std::string> args) {
    auto promise = std::make_shared<std::promise<AsyncRedisResult>>();
    ResultFuture future = promise->get_future();
    client.command(std::move(args), [promise](AsyncRedisResult result) mutable {
        promise->set_value(std::move(result));
    });
    return future;
}

bool await(ResultFuture& future, AsyncRedisResult& result) {
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return false;
    }
    result = future.get();
    return true;
}

bool expect(bool condition, const std::string& message, int& failures) {
    if (condition) {
        return true;
    }
    std::cerr << message << '\n';
    ++failures;
    return false;
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
        return 1;
    }

    int failures = 0;
    const std::string prefix = "myweb:test:async-client:";
    const std::string value("value\0with-binary", sizeof("value\0with-binary") - 1);
    AsyncRedisResult result;

    auto set = command(client, { "SETEX", prefix + "binary", "30", value });
    expect(await(set, result) && result.type == RedisResultType::Status && result.value == "OK",
        "binary SETEX did not return OK", failures);

    auto get = command(client, { "GET", prefix + "binary" });
    expect(await(get, result) && result.type == RedisResultType::String && result.value == value,
        "binary GET did not round-trip the exact bytes", failures);

    auto first_nx = command(client, { "SET", prefix + "nx", "first", "NX", "EX", "30" });
    expect(await(first_nx, result) && result.type == RedisResultType::Status && result.value == "OK",
        "first SET NX did not return OK", failures);

    auto second_nx = command(client, { "SET", prefix + "nx", "second", "NX", "EX", "30" });
    expect(await(second_nx, result) && result.type == RedisResultType::Nil,
        "second SET NX did not return nil", failures);

    auto eval = command(client, { "EVAL", "return 7", "0" });
    expect(await(eval, result) && result.type == RedisResultType::Integer && result.integer == 7,
        "Lua integer reply was not preserved", failures);

    auto remove = command(client, { "DEL", prefix + "binary", prefix + "nx" });
    expect(await(remove, result) && result.type == RedisResultType::Integer && result.integer == 2,
        "test keys were not deleted", failures);

    client.shutdown();
    if (failures != 0) {
        return 2;
    }
    std::cout << "async redis client test passed\n";
    return 0;
}
