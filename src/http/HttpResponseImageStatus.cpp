#include "http/HttpResponse.h"

#include "cache/ImageRedisService.h"

#include <string>
#include <utility>

void HttpResponse::build_image_status_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback
) {
    const std::string requested = url_decode(
        query_value(request.path(), "name")
    );
    const std::string name = safe_filename(requested);
    if (name.empty() || name != requested || !is_allowed_image_name(name)) {
        callback(build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"invalid image name\" }\n",
            keep_alive
        ));
        return;
    }

    ImageRedisService::instance().get_image_status(
        name,
        [this, name, keep_alive, callback = std::move(callback)](
            RedisCacheResult result
        ) mutable {
            if (request_cancelled()) {
                return;
            }
            if (result.state == RedisCacheState::Error) {
                callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"image status unavailable\" }\n",
                    keep_alive
                ));
                return;
            }

            std::string state = "MISSING";
            if (result.state == RedisCacheState::Hit) {
                state = std::move(result.value);
            }
            callback(build_json_response(
                200,
                "OK",
                "{ \"ok\": true, \"name\": \"" + json_escape(name) +
                    "\", \"status\": \"" + json_escape(state) + "\" }\n",
                keep_alive
            ));
        }
    );
}
