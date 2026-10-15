#include "auth/AsyncSessionManager.h"
#include "cache/ImageRedisService.h"
#include "cache/LRUCache.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "redis/AsyncRedisClient.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

template <typename Future>
bool ready(Future& future) {
    return future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}

bool invalidate_list() {
    std::promise<RedisWriteResult> promise;
    auto future = promise.get_future();
    ImageRedisService::instance().invalidate_image_list(
        [&promise](RedisWriteResult result) {
            promise.set_value(std::move(result));
        }
    );
    return ready(future) && future.get().ok;
}

std::string fetch_images(
    const std::string& token,
    const std::string& web_root,
    LRUCache& file_cache
) {
    HttpRequest request;
    request.parse(
        "GET /api/images HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: sid=" + token + "\r\n"
        "Connection: close\r\n\r\n"
    );
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    HttpResponse response(web_root, file_cache, 0, cancelled);
    std::promise<std::string> promise;
    auto future = promise.get_future();
    response.build_response_async(
        request,
        false,
        [&promise](std::string value) {
            promise.set_value(std::move(value));
        },
        [](std::function<void()> continuation) {
            continuation();
            return true;
        }
    );
    return ready(future) ? future.get() : "";
}

}

int main() {
    const char* enabled = std::getenv("RUN_REDIS_INTEGRATION_TESTS");
    if (!enabled || std::string(enabled) != "1") {
        std::cout << "set RUN_REDIS_INTEGRATION_TESTS=1 to run\n";
        return 77;
    }

    setenv("IMAGE_LIST_CACHE_TTL_SECONDS", "30", 1);
    if (!AsyncRedisClient::instance().init_from_env() ||
        !AsyncSessionManager::instance().init_from_env()) {
        std::cerr << "Redis initialization failed\n";
        return 1;
    }

    std::promise<std::string> create;
    auto create_future = create.get_future();
    AsyncSessionManager::instance().create_session(
        "cache_invalidation_user",
        "Cache Invalidation",
        [&create](std::string token, std::string error) {
            create.set_value(error.empty() ? std::move(token) : "");
        }
    );
    if (!ready(create_future)) {
        return 2;
    }
    std::string token = create_future.get();
    if (token.empty() || !invalidate_list()) {
        return 3;
    }

    namespace fs = std::filesystem;
    fs::path root = fs::path("/tmp") /
        ("myweb-cache-invalidation-" + std::to_string(static_cast<long long>(getpid())));
    fs::path images = root / "images";
    fs::create_directories(images);
    std::ofstream(images / "old.webp", std::ios::binary).put('x');

    LRUCache file_cache(1024 * 1024);
    std::string first = fetch_images(token, root.string(), file_cache);
    if (first.find("old.webp") == std::string::npos) {
        std::cerr << "first filesystem list did not contain old.webp\n";
        return 4;
    }

    if (!invalidate_list()) {
        return 5;
    }
    fs::remove(images / "old.webp");
    std::ofstream(images / "new.webp", std::ios::binary).put('y');

    std::string second = fetch_images(token, root.string(), file_cache);
    if (second.find("new.webp") == std::string::npos ||
        second.find("old.webp") != std::string::npos) {
        std::cerr << "Redis invalidation still returned the process-local stale list\n";
        return 6;
    }

    fs::remove_all(root);
    invalidate_list();
    std::promise<bool> destroy;
    auto destroy_future = destroy.get_future();
    AsyncSessionManager::instance().destroy_session(
        token,
        [&destroy](bool ok, std::string) {
            destroy.set_value(ok);
        }
    );
    if (!ready(destroy_future) || !destroy_future.get()) {
        return 7;
    }

    AsyncSessionManager::instance().shutdown();
    AsyncRedisClient::instance().shutdown();
    std::cout << "HTTP image cache invalidation test passed\n";
    return 0;
}
