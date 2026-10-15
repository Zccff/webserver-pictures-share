#include "auth/AsyncSessionManager.h"
#include "cache/ImageRedisService.h"
#include "cache/LRUCache.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "redis/AsyncRedisClient.h"
#include "utils/Metrics.h"
#include "db/UserStore.h"

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

uint64_t metric_value(const std::string& json, const std::string& name) {
    const std::string marker = "\"" + name + "\": ";
    const size_t start = json.find(marker);
    if (start == std::string::npos) {
        return 0;
    }
    const size_t value_start = start + marker.size();
    const size_t value_end = json.find_first_not_of("0123456789", value_start);
    return static_cast<uint64_t>(std::stoull(
        json.substr(value_start, value_end - value_start)
    ));
}

}

int main() {
    const char* enabled = std::getenv("RUN_REDIS_INTEGRATION_TESTS");
    if (!enabled || std::string(enabled) != "1") {
        std::cout << "set RUN_REDIS_INTEGRATION_TESTS=1 to run\n";
        return 77;
    }

    if (!AsyncRedisClient::instance().init_from_env() ||
        !AsyncSessionManager::instance().init_from_env()) {
        std::cerr << "Redis initialization failed\n";
        return 1;
    }

    const std::string cached_body =
        "{ \"ok\": true, \"images\": [{\"name\":\"redis-sentinel.webp\"}] }\n";
    std::promise<RedisWriteResult> cache_write;
    auto cache_write_future = cache_write.get_future();
    ImageRedisService::instance().put_image_list(
        cached_body,
        [&cache_write](RedisWriteResult result) {
            cache_write.set_value(std::move(result));
        }
    );
    if (!ready(cache_write_future) || !cache_write_future.get().ok) {
        std::cerr << "could not seed image-list cache\n";
        return 2;
    }

    std::promise<std::string> session_create;
    auto session_create_future = session_create.get_future();
    AsyncSessionManager::instance().create_session(
        "http_redis_test_user",
        "HTTP Redis Test",
        [&session_create](std::string token, std::string error) {
            session_create.set_value(error.empty() ? std::move(token) : "");
        }
    );
    if (!ready(session_create_future)) {
        std::cerr << "session creation timed out\n";
        return 3;
    }
    std::string token = session_create_future.get();
    if (token.empty()) {
        std::cerr << "session creation failed\n";
        return 4;
    }

    HttpRequest request;
    const std::string raw_request =
        "GET /api/images HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: sid=" + token + "\r\n"
        "Connection: close\r\n\r\n";
    if (!request.parse(raw_request)) {
        std::cerr << "request parsing failed\n";
        return 5;
    }

    LRUCache file_cache(1024 * 1024);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    HttpResponse response("/tmp/myweb-http-redis-empty", file_cache, 0, cancelled);
    std::promise<std::string> completed;
    auto completed_future = completed.get_future();
    response.build_response_async(
        request,
        false,
        [&completed](std::string value) {
            completed.set_value(std::move(value));
        },
        [](std::function<void()> continuation) {
            continuation();
            return true;
        }
    );

    if (!ready(completed_future)) {
        std::cerr << "HTTP response timed out\n";
        return 6;
    }
    std::string http_response = completed_future.get();
    if (http_response.find("redis-sentinel.webp") == std::string::npos) {
        std::cerr << "HTTP image route did not serve the Redis list cache\n";
        return 7;
    }


    auto run_write = [](auto start) {
        std::promise<RedisWriteResult> promise;
        auto future = promise.get_future();
        start([&promise](RedisWriteResult result) {
            promise.set_value(std::move(result));
        });
        if (!ready(future)) {
            return RedisWriteResult{ false, "ranking write timed out" };
        }
        return future.get();
    };
    const std::string ranking_name = "http-ranking-" +
        std::to_string(static_cast<long long>(getpid())) + ".webp";
    RedisWriteResult ranking_clear = run_write([&](auto callback) {
        ImageRedisService::instance().remove_image_rankings(
            ranking_name,
            std::move(callback)
        );
    });
    RedisWriteResult ranking_increment = run_write([&](auto callback) {
        ImageRedisService::instance().increment_image_view(
            ranking_name,
            std::move(callback)
        );
    });
    if (!ranking_clear.ok || !ranking_increment.ok) {
        std::cerr << "could not seed HTTP ranking\n";
        return 10;
    }

    HttpRequest ranking_request;
    ranking_request.parse(
        "GET /api/rankings?type=views&limit=20 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: sid=" + token + "\r\n"
        "Connection: close\r\n\r\n"
    );
    HttpResponse ranking_response(
        "/tmp/myweb-http-redis-empty",
        file_cache,
        0,
        cancelled
    );
    std::promise<std::string> ranking_completed;
    auto ranking_future = ranking_completed.get_future();
    ranking_response.build_response_async(
        ranking_request,
        false,
        [&ranking_completed](std::string value) {
            ranking_completed.set_value(std::move(value));
        },
        [](std::function<void()> continuation) {
            continuation();
            return true;
        }
    );
    if (!ready(ranking_future)) {
        std::cerr << "HTTP ranking response timed out\n";
        return 11;
    }
    const std::string ranking_http = ranking_future.get();
    if (ranking_http.find(ranking_name) == std::string::npos ||
        ranking_http.find("\"score\":1") == std::string::npos) {
        std::cerr << ranking_http;
        return 12;
    }
    run_write([&](auto callback) {
        ImageRedisService::instance().remove_image_rankings(
            ranking_name,
            std::move(callback)
        );
    });

    // Only an explicit view click counts. Loading the original image as a
    // gallery fallback must not alter the ranking; the same session may click
    // twice and contribute two points.
    char view_template[] = "/tmp/myweb-view-XXXXXX";
    char* view_temp = mkdtemp(view_template);
    if (!view_temp) {
        std::cerr << "could not create view fixture directory\n";
        return 21;
    }
    const std::filesystem::path view_root(view_temp);
    std::filesystem::create_directory(view_root / "images");
    const std::string view_name = "clicked-" +
        view_root.filename().string() + ".png";
    std::ofstream(view_root / "images" / view_name) << "test-image";

    auto call_view_http = [&](const std::string& raw) {
        HttpRequest view_request;
        if (!view_request.parse(raw)) return std::string("REQUEST_PARSE_FAILED");
        HttpResponse view_response(view_root.string(), file_cache, 0, cancelled);
        std::promise<std::string> view_completed;
        auto view_future = view_completed.get_future();
        view_response.build_response_async(
            view_request,
            false,
            [&view_completed](std::string value) {
                view_completed.set_value(std::move(value));
            },
            [](std::function<void()> continuation) {
                continuation();
                return true;
            }
        );
        if (!ready(view_future)) return std::string("VIEW_TIMEOUT");
        return view_future.get();
    };
    auto read_view_ranking = [&]() {
        std::promise<RedisRankingResult> ranking_promise;
        auto ranking_future = ranking_promise.get_future();
        ImageRedisService::instance().get_image_ranking(
            RedisRankingType::Views, 100,
            [&ranking_promise](RedisRankingResult result) {
                ranking_promise.set_value(std::move(result));
            }
        );
        if (!ready(ranking_future)) return std::string("RANKING_TIMEOUT");
        RedisRankingResult result = ranking_future.get();
        return result.ok ? result.json : std::string("RANKING_ERROR");
    };
    run_write([&](auto callback) {
        ImageRedisService::instance().remove_image_rankings(
            view_name, std::move(callback));
    });
    const std::string image_request =
        "GET /images/" + view_name + " HTTP/1.1\r\n"
        "Host: localhost\r\nCookie: sid=" + token +
        "\r\nConnection: close\r\n\r\n";
    const std::string passive_view = call_view_http(image_request);
    if (passive_view.find("HTTP/1.1 200 OK") != 0 ||
        read_view_ranking().find(view_name) != std::string::npos) {
        std::cerr << "passive image load altered click ranking\n";
        return 22;
    }
    const std::string click_request =
        "POST /api/image-view?name=" + view_name + " HTTP/1.1\r\n"
        "Host: localhost\r\nCookie: sid=" + token +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const std::string anonymous_click = call_view_http(
        "POST /api/image-view?name=" + view_name + " HTTP/1.1\r\n"
        "Host: localhost\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    const std::string invalid_click = call_view_http(
        "POST /api/image-view?name=..%2Fbad.png HTTP/1.1\r\n"
        "Host: localhost\r\nCookie: sid=" + token +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const std::string missing_click = call_view_http(
        "POST /api/image-view?name=missing-image.png HTTP/1.1\r\n"
        "Host: localhost\r\nCookie: sid=" + token +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    if (anonymous_click.find("HTTP/1.1 401 Unauthorized") != 0 ||
        invalid_click.find("HTTP/1.1 400 Bad Request") != 0 ||
        missing_click.find("HTTP/1.1 404 Not Found") != 0) {
        std::cerr << anonymous_click << invalid_click << missing_click;
        return 24;
    }
    const std::string first_click = call_view_http(click_request);
    const std::string second_click = call_view_http(click_request);
    const std::string view_ranking = read_view_ranking();
    if (first_click.find("HTTP/1.1 200 OK") != 0 ||
        second_click.find("HTTP/1.1 200 OK") != 0 ||
        view_ranking.find("\"name\":\"" + view_name +
            "\",\"score\":2") == std::string::npos) {
        std::cerr << "explicit clicks did not count twice: " << view_ranking
                  << "\n" << first_click << second_click;
        return 23;
    }
    run_write([&](auto callback) {
        ImageRedisService::instance().remove_image_rankings(
            view_name, std::move(callback));
    });
    std::filesystem::remove_all(view_root);

    // A content collision is rejected by the Redis lifecycle guard before the
    // MySQL duplicate branch runs. It must still be observable as a duplicate.
    const std::string duplicate_payload = "not-a-real-image";
    const std::string duplicate_name =
        UserStore::sha256("http_redis_test_user").substr(0, 12) + "_" +
        UserStore::sha256(duplicate_payload) + ".png";
    const std::string duplicate_generation = "existing-generation";
    RedisWriteResult lifecycle_seed = run_write([&](auto callback) {
        ImageRedisService::instance().begin_image_upload(
            duplicate_name,
            duplicate_generation,
            std::move(callback)
        );
    });
    if (!lifecycle_seed.ok || !lifecycle_seed.applied) {
        std::cerr << "could not seed duplicate image lifecycle\n";
        return 16;
    }

    const std::string boundary = "----MyWebDuplicateBoundary";
    const std::string duplicate_body =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"image\"; "
        "filename=\"duplicate.png\"\r\n"
        "Content-Type: image/png\r\n\r\n" +
        duplicate_payload + "\r\n--" + boundary + "--\r\n";
    HttpRequest duplicate_request;
    const std::string duplicate_raw =
        "POST /api/upload-image HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: sid=" + token + "\r\n"
        "Idempotency-Key: duplicate-http-" +
            std::to_string(static_cast<long long>(getpid())) + "\r\n"
        "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
        "Content-Length: " + std::to_string(duplicate_body.size()) +
            "\r\nConnection: close\r\n\r\n" + duplicate_body;
    if (!duplicate_request.parse(duplicate_raw)) {
        std::cerr << "duplicate upload request parsing failed\n";
        return 17;
    }

    const uint64_t duplicate_before = metric_value(
        Metrics::instance().to_json(0), "duplicate_uploads");
    HttpResponse duplicate_response(
        "/tmp/myweb-http-redis-empty", file_cache, 0, cancelled);
    std::promise<std::string> duplicate_completed;
    auto duplicate_future = duplicate_completed.get_future();
    duplicate_response.build_response_async(
        duplicate_request,
        false,
        [&duplicate_completed](std::string value) {
            duplicate_completed.set_value(std::move(value));
        },
        [](std::function<void()> continuation) {
            continuation();
            return true;
        }
    );
    if (!ready(duplicate_future)) {
        std::cerr << "duplicate upload response timed out\n";
        return 18;
    }
    const std::string duplicate_http = duplicate_future.get();
    const uint64_t duplicate_after = metric_value(
        Metrics::instance().to_json(0), "duplicate_uploads");
    if (duplicate_http.find("HTTP/1.1 409 Conflict") != 0 ||
        duplicate_after != duplicate_before + 1) {
        std::cerr << duplicate_http;
        return 19;
    }
    RedisWriteResult lifecycle_cleanup = run_write([&](auto callback) {
        ImageRedisService::instance().abort_image_upload(
            duplicate_name,
            duplicate_generation,
            std::move(callback)
        );
    });
    if (!lifecycle_cleanup.ok) {
        std::cerr << "could not clean duplicate image lifecycle\n";
        return 20;
    }

    // DELETE must enter DeleteRedisFlow. The synchronous fallback deliberately
    // returns "requires async response path", so this catches routing regressions
    // without requiring a MySQL fixture for this Redis-focused test.
    HttpRequest delete_request;
    if (!delete_request.parse(
            "DELETE /api/images?name=does-not-exist.webp HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Cookie: sid=" + token + "\r\n"
            "Connection: close\r\n\r\n")) {
        std::cerr << "delete request parsing failed\n";
        return 13;
    }
    HttpResponse delete_response(
        "/tmp/myweb-http-redis-empty",
        file_cache,
        0,
        cancelled
    );
    std::promise<std::string> delete_completed;
    auto delete_future = delete_completed.get_future();
    delete_response.build_response_async(
        delete_request,
        false,
        [&delete_completed](std::string value) {
            delete_completed.set_value(std::move(value));
        },
        [](std::function<void()> continuation) {
            continuation();
            return true;
        }
    );
    if (!ready(delete_future)) {
        std::cerr << "HTTP delete response timed out\n";
        return 14;
    }
    const std::string delete_http = delete_future.get();
    if (delete_http.find("image deletion requires async response path") !=
            std::string::npos ||
        delete_http.find("image database is busy or unavailable") ==
            std::string::npos) {
        std::cerr << delete_http;
        return 15;
    }

    std::promise<bool> session_destroy;
    auto session_destroy_future = session_destroy.get_future();
    AsyncSessionManager::instance().destroy_session(
        token,
        [&session_destroy](bool ok, std::string) {
            session_destroy.set_value(ok);
        }
    );
    if (!ready(session_destroy_future) || !session_destroy_future.get()) {
        std::cerr << "session cleanup failed\n";
        return 8;
    }

    std::promise<RedisWriteResult> invalidate;
    auto invalidate_future = invalidate.get_future();
    ImageRedisService::instance().invalidate_image_list(
        [&invalidate](RedisWriteResult result) {
            invalidate.set_value(std::move(result));
        }
    );
    if (!ready(invalidate_future) || !invalidate_future.get().ok) {
        std::cerr << "cache cleanup failed\n";
        return 9;
    }

    AsyncSessionManager::instance().shutdown();
    AsyncRedisClient::instance().shutdown();
    std::cout << "HTTP image Redis integration test passed\n";
    return 0;
}
