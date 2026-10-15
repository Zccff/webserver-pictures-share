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
    return future.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
}

std::string fetch(
    const std::string& path,
    const std::string& token,
    const std::string& web_root,
    LRUCache& file_cache
) {
    HttpRequest request;
    request.parse(
        "GET " + path + " HTTP/1.1\r\n"
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
    if (std::string(std::getenv("RUN_REDIS_INTEGRATION_TESTS")
            ? std::getenv("RUN_REDIS_INTEGRATION_TESTS") : "") != "1") {
        return 77;
    }
    setenv("X_ACCEL_REDIRECT", "1", 1);
    if (!AsyncRedisClient::instance().init_from_env() ||
        !AsyncSessionManager::instance().init_from_env()) {
        return 1;
    }

    std::promise<std::string> create;
    auto create_future = create.get_future();
    AsyncSessionManager::instance().create_session(
        "thumbnail_test_user",
        "Thumbnail Test",
        [&create](std::string token, std::string error) {
            create.set_value(error.empty() ? std::move(token) : "");
        }
    );
    if (!ready(create_future)) {
        return 2;
    }
    const std::string token = create_future.get();

    namespace fs = std::filesystem;
    fs::path root = fs::path("/tmp") /
        ("myweb-thumbnail-" +
         std::to_string(static_cast<long long>(getpid())));
    fs::create_directories(root / "images" / "thumbnails");
    std::ofstream(root / "images" / "sample.jpg", std::ios::binary).put('o');
    std::ofstream(
        root / "images" / "thumbnails" / "sample.webp",
        std::ios::binary
    ).put('t');

    std::promise<RedisWriteResult> invalidate;
    auto invalidate_future = invalidate.get_future();
    ImageRedisService::instance().invalidate_image_list(
        [&invalidate](RedisWriteResult result) {
            invalidate.set_value(std::move(result));
        }
    );
    if (!ready(invalidate_future) || !invalidate_future.get().ok) {
        return 3;
    }

    LRUCache cache(1024 * 1024);
    const std::string list = fetch("/api/images", token, root.string(), cache);
    if (list.find("\"thumbnail_url\": \"/thumbnails/sample.webp\"") ==
            std::string::npos ||
        list.find("\"thumbnail_status\": \"READY\"") ==
            std::string::npos) {
        std::cerr << list;
        return 4;
    }

    std::promise<RedisWriteResult> put_status;
    auto put_status_future = put_status.get_future();
    ImageRedisService::instance().put_image_status(
        "sample.jpg",
        "PROCESSING",
        [&put_status](RedisWriteResult result) {
            put_status.set_value(std::move(result));
        }
    );
    if (!ready(put_status_future) || !put_status_future.get().ok) {
        return 5;
    }

    const std::string status_response = fetch(
        "/api/image-status?name=sample.jpg",
        token,
        root.string(),
        cache
    );
    if (status_response.find("\"status\": \"PROCESSING\"") ==
            std::string::npos) {
        std::cerr << status_response;
        return 6;
    }

    const std::string thumbnail = fetch(
        "/thumbnails/sample.webp",
        token,
        root.string(),
        cache
    );
    if (thumbnail.find("HTTP/1.1 200 OK") == std::string::npos ||
        thumbnail.find(
            "X-Accel-Redirect: /protected-thumbnails/sample.webp"
        ) == std::string::npos) {
        std::cerr << thumbnail;
        return 5;
    }

    fs::remove_all(root);
    std::promise<bool> destroy;
    auto destroy_future = destroy.get_future();
    AsyncSessionManager::instance().destroy_session(
        token,
        [&destroy](bool ok, std::string) { destroy.set_value(ok); }
    );
    if (!ready(destroy_future) || !destroy_future.get()) {
        return 6;
    }
    AsyncSessionManager::instance().shutdown();
    AsyncRedisClient::instance().shutdown();
    return 0;
}
