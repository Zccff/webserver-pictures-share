#include "http/HttpResponse.h"

#include "cache/ImageRedisService.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

void HttpResponse::build_rankings_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback
) {
    std::string type = query_value(request.path(), "type");
    RedisRankingType ranking_type = RedisRankingType::Views;
    std::string type_name = "views";
    if (type.empty() || type == "view" || type == "views" ||
        type == "click" || type == "clicks") {
        ranking_type = RedisRankingType::Views;
    }
    else if (type == "download" || type == "downloads") {
        ranking_type = RedisRankingType::Downloads;
        type_name = "downloads";
    }
    else {
        callback(build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"type must be views or downloads\" }\n",
            keep_alive
        ));
        return;
    }

    size_t limit = 20;
    const std::string limit_text = query_value(request.path(), "limit");
    if (!limit_text.empty()) {
        char* end = nullptr;
        errno = 0;
        unsigned long parsed = std::strtoul(limit_text.c_str(), &end, 10);
        if (errno == ERANGE || end == limit_text.c_str() ||
            (end && *end != '\0') || parsed == 0 || parsed > 100) {
            callback(build_json_response(
                400,
                "Bad Request",
                "{ \"ok\": false, \"message\": \"limit must be between 1 and 100\" }\n",
                keep_alive
            ));
            return;
        }
        limit = static_cast<size_t>(parsed);
    }

    ImageRedisService::instance().get_image_ranking(
        ranking_type,
        limit,
        [this,
         type_name = std::move(type_name),
         keep_alive,
         callback = std::move(callback)](RedisRankingResult result) mutable {
            if (request_cancelled()) {
                return;
            }
            if (!result.ok) {
                callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"image ranking unavailable\" }\n",
                    keep_alive
                ));
                return;
            }
            callback(build_json_response(
                200,
                "OK",
                "{ \"ok\": true, \"type\": \"" + type_name +
                    "\", \"rankings\": " + result.json + " }\n",
                keep_alive
            ));
        }
    );
}

void HttpResponse::build_record_image_view_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback
) {
    const std::string raw_name = url_decode(query_value(request.path(), "name"));
    const std::string name = safe_filename(raw_name);
    if (raw_name.empty() || raw_name != name || name.size() > 200 ||
        !is_allowed_image_name(name)) {
        callback(build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"invalid image name\" }\n",
            keep_alive
        ));
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(images_dir()) / name, error) || error) {
        callback(build_404_response(keep_alive));
        return;
    }

    ImageRedisService::instance().increment_image_view(
        name,
        [this, keep_alive, callback = std::move(callback)](
            RedisWriteResult result) mutable {
            if (request_cancelled()) {
                return;
            }
            if (!result.ok) {
                callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"image ranking unavailable\" }\n",
                    keep_alive
                ));
                return;
            }
            if (!result.applied) {
                callback(build_404_response(keep_alive));
                return;
            }
            callback(build_json_response(
                200,
                "OK",
                "{ \"ok\": true }\n",
                keep_alive
            ));
        }
    );
}
