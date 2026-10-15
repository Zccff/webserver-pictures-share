#include "http/HttpResponse.h"

#include "auth/AsyncSessionManager.h"
#include "cache/ImageRedisService.h"
#include "db/UserStore.h"
#include "image/ImageProcessor.h"
#include "log/AsyncLogger.h"
#include "utils/Metrics.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uintmax_t MAX_MEMORY_CACHED_FILE_SIZE = 256 * 1024;

size_t positive_env_size(const char* name, size_t fallback) {
    const char* value = std::getenv(name);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
    return parsed == 0 ? fallback : parsed;
}

size_t max_images_per_user() {
    static const size_t value = positive_env_size("MAX_IMAGES_PER_USER", 100);
    return value;
}

std::string thumbnail_name_for(const std::string& image_name) {
    std::filesystem::path path(image_name);
    return path.stem().string() + ".webp";
}

bool has_control_character(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

std::string validate_registration_fields(
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    const std::string& email
) {
    if (username.size() < 3 || username.size() > 64) {
        return "username must contain 3 to 64 characters";
    }

    if (!std::all_of(username.begin(), username.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    })) {
        return "username may only contain letters, digits, underscore, and hyphen";
    }

    if (password.size() < 8 || password.size() > 128) {
        return "password must contain 8 to 128 characters";
    }

    if (display_name.size() > 64 || has_control_character(display_name)) {
        return "display name must be at most 64 characters without control characters";
    }

    if (email.size() > 128 || has_control_character(email)) {
        return "email must be at most 128 characters without control characters";
    }

    if (!email.empty()) {
        size_t at = email.find('@');

        if (at == std::string::npos || at == 0 || at + 1 >= email.size() ||
            email.find('.', at + 1) == std::string::npos) {
            return "email format is invalid";
        }
    }

    return "";
}

}

HttpResponse::HttpResponse(
    std::string web_root,
    LRUCache& file_cache,
    size_t thread_pool_queue_size,
    std::shared_ptr<std::atomic<bool>> cancelled
)
    : web_root_(std::move(web_root)),
    file_cache_(file_cache),
    thread_pool_queue_size_(thread_pool_queue_size),
    cancelled_(std::move(cancelled)) {
}

std::string HttpResponse::build_response(
    const HttpRequest& request,
    bool keep_alive
) {
    std::string clean_path = path_without_query(request.path());

    if (starts_with(clean_path, "/api/")) {
        return build_api_response(request, nullptr, keep_alive);
    }

    if (starts_with(clean_path, "/images/") ||
        starts_with(clean_path, "/thumbnails/")) {
        return build_json_response(
            401,
            "Unauthorized",
            "{ \"ok\": false, \"message\": \"login required\" }\n",
            keep_alive
        );
    }

    return build_response(request.method(), request.path(), keep_alive);
}

void HttpResponse::build_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    if (request_cancelled()) {
        return;
    }

    std::string clean_path = path_without_query(request.path());

    if (request.method() == "POST" && clean_path == "/api/login") {
        build_login_response_async(
            request,
            keep_alive,
            std::move(callback),
            std::move(executor)
        );
        return;
    }

    if (request.method() == "POST" && clean_path == "/api/logout") {
        build_logout_response_async(
            request,
            keep_alive,
            std::move(callback),
            std::move(executor)
        );
        return;
    }

    if (!requires_session(request)) {
        callback(build_response(request, keep_alive));
        return;
    }

    std::string token = cookie_value(request, "sid");

    if (token.empty()) {
        callback(build_json_response(
            401,
            "Unauthorized",
            "{ \"ok\": false, \"message\": \"login required\" }\n",
            keep_alive
        ));
        return;
    }

    AsyncSessionManager::instance().get_session(
        token,
        [
            this,
            request,
            keep_alive,
            callback = std::move(callback),
            executor = std::move(executor)
        ](bool found, SessionInfo session, std::string error) mutable {
            std::string redis_path = path_without_query(request.path());
            if (error.empty() && found &&
                ((request.method() == "GET" && redis_path == "/api/images") ||
                 (request.method() == "GET" && redis_path == "/api/image-status") ||
                 (request.method() == "GET" && redis_path == "/api/rankings") ||
                 (request.method() == "POST" &&
                  redis_path == "/api/image-view") ||
                 (request.method() == "POST" && redis_path == "/api/upload-image") ||
                 (request.method() == "DELETE" &&
                  redis_path == "/api/images"))) {
                build_authenticated_response_async(request, session, keep_alive, callback, executor);
                return;
            }
            CompletionCallback scheduled_callback = callback;
            bool scheduled = executor([
                this,
                request,
                found,
                session = std::move(session),
                error = std::move(error),
                keep_alive,
                callback = std::move(scheduled_callback)
            ]() mutable {
                if (!error.empty()) {
                    callback(build_json_response(
                        503,
                        "Service Unavailable",
                        "{ \"ok\": false, \"message\": \"session store unavailable\" }\n",
                        keep_alive
                    ));
                    return;
                }

                if (!found) {
                    callback(build_json_response(
                        401,
                        "Unauthorized",
                        "{ \"ok\": false, \"message\": \"login required\" }\n",
                        keep_alive
                    ));
                    return;
                }

                callback(build_authenticated_response(request, session, keep_alive));
            });

            if (!scheduled) {
                callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n",
                    keep_alive
                ));
            }
        }
    );
}

std::string HttpResponse::build_response(
    const std::string& method,
    const std::string& request_path,
    bool keep_alive
) {
    std::string clean_path = path_without_query(request_path);

    if (method == "HEAD") {
        std::string response = without_body(build_response("GET", clean_path, keep_alive));
        send_file_path_.clear();
        send_file_size_ = 0;
        return response;
    }

    if (method != "GET") {
        return build_405_response(keep_alive);
    }

    if (clean_path == "/metrics") {
        return build_metrics_response(keep_alive);
    }

    return build_static_response(clean_path, keep_alive);
}

std::string HttpResponse::build_static_response(
    const std::string& request_path,
    bool keep_alive
) {
    std::string path = normalize_path(request_path);

    if (path.empty()) {
        return build_404_response(keep_alive);
    }

    std::string file_path = web_root_ + path;

    return build_file_response(file_path, keep_alive);
}

std::string HttpResponse::build_file_response(
    const std::string& file_path,
    bool keep_alive,
    const std::string& extra_headers
) {
    namespace fs = std::filesystem;
    std::error_code ec;
    uintmax_t file_size = fs::file_size(file_path, ec);

    if (ec || !fs::is_regular_file(file_path, ec) || ec) {
        return build_404_response(keep_alive);
    }

    if (file_size > MAX_MEMORY_CACHED_FILE_SIZE) {
        send_file_path_ = file_path;
        send_file_size_ = static_cast<uint64_t>(file_size);
        return build_response_headers(
            200,
            "OK",
            get_content_type(file_path),
            send_file_size_,
            keep_alive,
            extra_headers
        );
    }

    LRUCache::Value body;

    if (!read_file_with_cache(file_path, body)) {
        return build_404_response(keep_alive);
    }

    return build_raw_response(
        200,
        "OK",
        get_content_type(file_path),
        *body,
        keep_alive,
        extra_headers
    );
}

bool HttpResponse::has_send_file() const {
    return !send_file_path_.empty();
}

const std::string& HttpResponse::send_file_path() const {
    return send_file_path_;
}

uint64_t HttpResponse::send_file_size() const {
    return send_file_size_;
}

std::string HttpResponse::build_protected_image_response(
    const std::string& request_path,
    bool keep_alive
) {
    std::string prefix = "/images/";

    if (!starts_with(request_path, prefix)) {
        return build_404_response(keep_alive);
    }

    std::string name = safe_filename(url_decode(request_path.substr(prefix.size())));

    if (name.empty() || !is_allowed_image_name(name)) {
        return build_404_response(keep_alive);
    }

    const std::filesystem::path image_path =
        std::filesystem::path(images_dir()) / name;
    if (!std::filesystem::is_regular_file(image_path)) {
        return build_404_response(keep_alive);
    }
    if (use_x_accel_redirect()) {
        return build_accel_image_response(name, false, keep_alive);
    }

    return build_static_response("/images/" + name, keep_alive);
}

std::string HttpResponse::build_protected_thumbnail_response(
    const std::string& request_path,
    bool keep_alive
) {
    const std::string prefix = "/thumbnails/";
    if (!starts_with(request_path, prefix)) {
        return build_404_response(keep_alive);
    }
    const std::string name =
        safe_filename(url_decode(request_path.substr(prefix.size())));
    if (name.empty() || !has_suffix(name, ".webp")) {
        return build_404_response(keep_alive);
    }
    if (use_x_accel_redirect()) {
        return build_accel_thumbnail_response(name, keep_alive);
    }
    return build_file_response(
        (std::filesystem::path(images_dir()) / "thumbnails" / name).string(),
        keep_alive
    );
}

std::string HttpResponse::build_accel_image_response(
    const std::string& name,
    bool download,
    bool keep_alive
) {
    namespace fs = std::filesystem;

    fs::path file_path = fs::path(images_dir()) / name;

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        return build_404_response(keep_alive);
    }

    std::string extra_headers =
        "X-Accel-Redirect: /protected-images/" + name + "\r\n"
        "Cache-Control: private, no-store\r\n";

    if (download) {
        extra_headers += "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
    }

    return build_raw_response(
        200,
        "OK",
        get_content_type(file_path.string()),
        "",
        keep_alive,
        extra_headers
    );
}

std::string HttpResponse::build_accel_thumbnail_response(
    const std::string& name,
    bool keep_alive
) {
    namespace fs = std::filesystem;
    fs::path file_path = fs::path(images_dir()) / "thumbnails" / name;
    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        return build_404_response(keep_alive);
    }
    const std::string extra_headers =
        "X-Accel-Redirect: /protected-thumbnails/" + name + "\r\n"
        "Cache-Control: private, max-age=300\r\n";
    return build_raw_response(
        200,
        "OK",
        "image/webp",
        "",
        keep_alive,
        extra_headers
    );
}

std::string HttpResponse::build_metrics_response(bool keep_alive) {
    std::string body = Metrics::instance().to_json(thread_pool_queue_size_);

    return build_raw_response(
        200,
        "OK",
        "application/json; charset=utf-8",
        body,
        keep_alive
    );
}

std::string HttpResponse::build_api_response(
    const HttpRequest& request,
    bool keep_alive
) {
    return build_api_response(request, nullptr, keep_alive);
}

std::string HttpResponse::build_api_response(
    const HttpRequest& request,
    const SessionInfo* session,
    bool keep_alive
) {
    std::string clean_path = path_without_query(request.path());

    if (request.method() == "POST" && clean_path == "/api/register") {
        return build_register_response(request, keep_alive);
    }

    if (request.method() == "POST" && clean_path == "/api/login") {
        return build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"login requires async response path\" }\n",
            keep_alive
        );
    }

    if (request.method() == "POST" && clean_path == "/api/logout") {
        return build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"logout requires async response path\" }\n",
            keep_alive
        );
    }

    if (request.method() == "GET" && clean_path == "/api/me") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_me_response(*session, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/images") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_images_response(keep_alive);
    }

    if (request.method() == "POST" && clean_path == "/api/image-view") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }
        return build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"image view requires async response path\" }\n",
            keep_alive
        );
    }

    if (request.method() == "POST" && clean_path == "/api/upload-image") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"image upload requires async response path\" }\n",
            keep_alive
        );
    }

    if (request.method() == "DELETE" && clean_path == "/api/images") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_delete_image_response(request, *session, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/download") {
        if (!session) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_download_response(request, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/db-status") {
        return build_db_status_response(keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/session-status") {
        return build_session_status_response(keep_alive);
    }

    if (request.method() != "GET" && request.method() != "POST" &&
        request.method() != "DELETE") {
        return build_405_response(keep_alive);
    }

    return build_json_response(
        404,
        "Not Found",
        "{ \"ok\": false, \"message\": \"api endpoint not found\" }\n",
        keep_alive
    );
}

std::string HttpResponse::build_authenticated_response(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive
) {
    std::string clean_path = path_without_query(request.path());

    if (starts_with(clean_path, "/images/")) {
        return build_protected_image_response(clean_path, keep_alive);
    }

    if (starts_with(clean_path, "/thumbnails/")) {
        return build_protected_thumbnail_response(clean_path, keep_alive);
    }

    return build_api_response(request, &session, keep_alive);
}

std::string HttpResponse::build_register_response(
    const HttpRequest& request,
    bool keep_alive
) {
    auto form = parse_urlencoded(request.body());

    if (request.body().size() > 4096) {
        return build_json_response(
            413,
            "Payload Too Large",
            "{ \"ok\": false, \"message\": \"registration form is too large\" }\n",
            keep_alive
        );
    }

    std::string validation_error = validate_registration_fields(
        form["username"],
        form["password"],
        form["display_name"],
        form["email"]
    );

    if (!validation_error.empty()) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"" +
                json_escape(validation_error) + "\" }\n",
            keep_alive
        );
    }

    RegisterResult result = UserStore::instance().register_user(
        form["username"],
        form["password"],
        form["display_name"],
        form["email"],
        cancelled_
    );

    if (request_cancelled()) {
        return "";
    }

    int status = 500;
    std::string status_text = "Internal Server Error";
    std::string message = "registration failed";

    if (result.status == UserResultStatus::Ok) {
        status = 200;
        status_text = "OK";
        message = result.message;
    }
    else if (result.status == UserResultStatus::Conflict) {
        status = 409;
        status_text = "Conflict";
        message = result.message;
    }
    else if (result.status == UserResultStatus::InvalidInput) {
        status = 400;
        status_text = "Bad Request";
        message = result.message;
    }
    else if (result.status == UserResultStatus::Unavailable) {
        status = 503;
        status_text = "Service Unavailable";
        message = "user database is busy or unavailable";
    }
    else if (result.status == UserResultStatus::Cancelled) {
        return "";
    }
    else {
        LOG_ERROR("register user failed: " + result.message);
    }

    std::ostringstream body;
    body << "{ \"ok\": " << (result.ok ? "true" : "false")
         << ", \"message\": \"" << json_escape(message) << "\" }\n";

    return build_json_response(
        status,
        status_text,
        body.str(),
        keep_alive
    );
}

std::string HttpResponse::build_login_response(
    const HttpRequest& request,
    bool keep_alive
) {
    (void)request;
    return build_json_response(
        503,
        "Service Unavailable",
        "{ \"ok\": false, \"message\": \"login requires async response path\" }\n",
        keep_alive
    );
}

void HttpResponse::build_login_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    auto form = parse_urlencoded(request.body());

    if (request.body().size() > 4096 || form["username"].empty() ||
        form["username"].size() > 64 || form["password"].empty() ||
        form["password"].size() > 128) {
        callback(build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"invalid username or password format\" }\n",
            keep_alive
        ));
        return;
    }

    LoginResult result = UserStore::instance().login_user(
        form["username"],
        form["password"]
    );

    if (request_cancelled()) {
        return;
    }

    if (!result.ok) {
        int status = 500;
        std::string status_text = "Internal Server Error";
        std::string message = "login failed";

        if (result.status == UserResultStatus::InvalidCredentials ||
            result.status == UserResultStatus::InvalidInput) {
            status = 401;
            status_text = "Unauthorized";
            message = result.message;
        }
        else if (result.status == UserResultStatus::Unavailable) {
            status = 503;
            status_text = "Service Unavailable";
            message = "user database is busy or unavailable";
        }
        else {
            LOG_ERROR("login user failed: " + result.message);
        }

        std::ostringstream body;
        body << "{ \"ok\": false, \"message\": \""
             << json_escape(message) << "\" }\n";
        callback(build_json_response(
            status,
            status_text,
            body.str(),
            keep_alive
        ));
        return;
    }

    AsyncSessionManager::instance().create_session(
        result.username,
        result.display_name,
        [
            this,
            result,
            keep_alive,
            callback = std::move(callback),
            executor = std::move(executor)
        ](
            std::string token,
            std::string error
        ) mutable {
            CompletionCallback fallback_callback = callback;
            bool scheduled = executor([
                this,
                result,
                token = std::move(token),
                error = std::move(error),
                keep_alive,
                callback = std::move(callback)
            ]() mutable {
                if (token.empty()) {
                    callback(build_json_response(
                        503,
                        "Service Unavailable",
                        "{ \"ok\": false, \"message\": \"session store unavailable: " +
                            json_escape(error) + "\" }\n",
                        keep_alive
                    ));
                    return;
                }

                std::ostringstream body;
                body << "{ \"ok\": true, \"message\": \""
                     << json_escape(result.message) << "\", \"user\": {"
                     << "\"username\": \"" << json_escape(result.username) << "\", "
                     << "\"display_name\": \"" << json_escape(result.display_name) << "\""
                     << " } }\n";

                callback(build_raw_response(
                    200,
                    "OK",
                    "application/json; charset=utf-8",
                    body.str(),
                    keep_alive,
                    "Set-Cookie: sid=" + token + "; Path=/; HttpOnly; SameSite=Lax\r\n"
                ));
            });

            if (!scheduled) {
                fallback_callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n",
                    keep_alive
                ));
            }
        }
    );
}

std::string HttpResponse::build_logout_response(
    const HttpRequest& request,
    bool keep_alive
) {
    (void)request;
    return build_raw_response(
        503,
        "Service Unavailable",
        "application/json; charset=utf-8",
        "{ \"ok\": false, \"message\": \"logout requires async response path\" }\n",
        keep_alive,
        ""
    );
}

void HttpResponse::build_logout_response_async(
    const HttpRequest& request,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    std::string token = cookie_value(request, "sid");

    AsyncSessionManager::instance().destroy_session(
        token,
        [
            this,
            keep_alive,
            callback = std::move(callback),
            executor = std::move(executor)
        ](
            bool ok,
            std::string error
        ) mutable {
            CompletionCallback fallback_callback = callback;
            bool scheduled = executor([
                this,
                ok,
                error = std::move(error),
                keep_alive,
                callback = std::move(callback)
            ]() mutable {
                std::string headers =
                    "Set-Cookie: sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax\r\n";

                if (!ok) {
                    callback(build_raw_response(
                        503,
                        "Service Unavailable",
                        "application/json; charset=utf-8",
                        "{ \"ok\": false, \"message\": \"session deletion failed: " +
                            json_escape(error) + "\" }\n",
                        keep_alive,
                        headers
                    ));
                    return;
                }

                callback(build_raw_response(
                    200,
                    "OK",
                    "application/json; charset=utf-8",
                    "{ \"ok\": true, \"message\": \"logged out\" }\n",
                    keep_alive,
                    headers
                ));
            });

            if (!scheduled) {
                fallback_callback(build_json_response(
                    503,
                    "Service Unavailable",
                    "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n",
                    keep_alive
                ));
            }
        }
    );
}

std::string HttpResponse::build_me_response(
    const SessionInfo& session,
    bool keep_alive
) {
    std::ostringstream body;
    body << "{ \"ok\": true, \"user\": {"
         << "\"username\": \"" << json_escape(session.username) << "\", "
         << "\"display_name\": \"" << json_escape(session.display_name) << "\""
         << "} }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
}

std::string HttpResponse::build_images_response(bool keep_alive) {
    namespace fs = std::filesystem;

    fs::path images_dir = fs::path(web_root_) / "images";
    std::vector<std::string> images;

    if (fs::exists(images_dir) && fs::is_directory(images_dir)) {
        for (const auto& entry : fs::directory_iterator(images_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::string name = entry.path().filename().string();

            if (
                has_suffix(name, ".png") ||
                has_suffix(name, ".jpg") ||
                has_suffix(name, ".jpeg") ||
                has_suffix(name, ".webp") ||
                has_suffix(name, ".gif")
            ) {
                images.push_back(name);
            }
        }
    }

    std::sort(images.begin(), images.end());

    std::ostringstream body;
    body << "{ \"ok\": true, \"images\": [";

    for (size_t i = 0; i < images.size(); ++i) {
        if (i > 0) {
            body << ", ";
        }

        const std::string thumbnail_name = thumbnail_name_for(images[i]);
        const bool thumbnail_ready = fs::is_regular_file(
            images_dir / "thumbnails" / thumbnail_name
        );
        const std::string thumbnail_url = thumbnail_ready
            ? "/thumbnails/" + thumbnail_name
            : "/images/" + images[i];
        body << "{"
             << "\"name\": \"" << json_escape(images[i]) << "\", "
             << "\"url\": \"/images/" << json_escape(images[i]) << "\", "
             << "\"thumbnail_url\": \"" << json_escape(thumbnail_url) << "\", "
             << "\"thumbnail_status\": \""
             << (thumbnail_ready ? "READY" : "MISSING") << "\", "
             << "\"download_url\": \"/api/download?name=" << json_escape(images[i]) << "\""
             << ", \"delete_url\": \"/api/images?name=" << json_escape(images[i]) << "\""
             << "}";
    }

    body << "] }\n";

    std::string body_text = body.str();

    return build_json_response(200, "OK", body_text, keep_alive);
}

std::string HttpResponse::build_upload_image_response(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive,
    const std::string& precomputed_hash
) {
    if (request_cancelled()) {
        return "";
    }


    std::string filename;
    std::string content;
    std::string error;

    const bool streamed_upload = request.has_streamed_upload();
    const std::string streamed_path = request.upload_temp_path();
    uint64_t upload_size = streamed_upload ? request.upload_size() : 0;

    if (streamed_upload) {
        filename = url_decode(request.upload_original_name());
    }
    else if (!parse_multipart_image(request, filename, content, error)) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"" + json_escape(error) + "\" }\n",
            keep_alive
        );
    }
    if (!streamed_upload) {
        upload_size = static_cast<uint64_t>(content.size());
    }

    std::string safe_name = safe_filename(filename);

    if (safe_name.empty() || safe_name.size() > 200 ||
        !is_allowed_image_name(safe_name)) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"invalid image filename or extension\" }\n",
            keep_alive
        );
    }

    if (streamed_upload
            ? !image_file_matches_name(safe_name, streamed_path)
            : !image_content_matches_name(safe_name, content)) {
        return build_json_response(
            415,
            "Unsupported Media Type",
            "{ \"ok\": false, \"message\": \"file content does not match its image extension\" }\n",
            keep_alive
        );
    }

    std::string content_hash = precomputed_hash;
    if (content_hash.empty() && streamed_upload) {
        if (!Sha256::digest_file(streamed_path, content_hash, error)) {
            return build_json_response(
                500,
                "Internal Server Error",
                "{ \"ok\": false, \"message\": \"" +
                    json_escape(error) + "\" }\n",
                keep_alive
            );
        }
    }
    else if (content_hash.empty()) {
        content_hash = UserStore::sha256(content);
    }

    if (request_cancelled()) {
        return "";
    }
    size_t dot = safe_name.find_last_of('.');
    std::string extension = safe_name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::string saved_name = UserStore::sha256(session.username).substr(0, 12) +
        "_" + content_hash + extension;

    ImageReserveResult reserve = UserStore::instance().reserve_image(
        session.username,
        saved_name,
        safe_name,
        content_hash,
        upload_size,
        max_images_per_user(),
        cancelled_
    );

    if (reserve.status != ImageReserveStatus::Ok) {
        if (reserve.status == ImageReserveStatus::Cancelled) {
            return "";
        }

        int status = 500;
        std::string text = "Internal Server Error";
        std::string message = reserve.message;

        if (reserve.status == ImageReserveStatus::Duplicate) {
            Metrics::instance().inc_duplicate_upload();
            status = 409;
            text = "Conflict";
        }
        else if (reserve.status == ImageReserveStatus::LimitExceeded) {
            Metrics::instance().inc_rate_limited_upload();
            status = 429;
            text = "Too Many Requests";
        }
        else if (reserve.status == ImageReserveStatus::Unavailable) {
            status = 503;
            text = "Service Unavailable";
            message = "image database is busy or unavailable";
        }
        else {
            LOG_ERROR("reserve image metadata failed: " + reserve.message);
            message = "could not save image metadata";
        }

        return build_json_response(
            status,
            text,
            "{ \"ok\": false, \"message\": \"" +
                json_escape(message) + "\" }\n",
            keep_alive
        );
    }

    if (request_cancelled()) {
        UserStore::instance().delete_owned_image(session.username, saved_name);
        return "";
    }

    const bool published = streamed_upload
        ? publish_streamed_upload(saved_name, streamed_path, error)
        : write_uploaded_image(saved_name, content, error);
    if (!published) {
        UserStore::instance().delete_owned_image(session.username, saved_name);
        return build_json_response(
            500,
            "Internal Server Error",
            "{ \"ok\": false, \"message\": \"" + json_escape(error) + "\" }\n",
            keep_alive
        );
    }

    if (request_cancelled()) {
        std::error_code remove_error;
        std::filesystem::remove(
            std::filesystem::path(images_dir()) / saved_name,
            remove_error
        );
        UserStore::instance().delete_owned_image(session.username, saved_name);
        return "";
    }

    std::string thumbnail_status = "PROCESSING";

    std::ostringstream body;
    body << "{ \"ok\": true, \"message\": \"uploaded\", "
         << "\"file\": {"
         << "\"name\": \"" << json_escape(saved_name) << "\", "
         << "\"url\": \"/images/" << json_escape(saved_name) << "\", "
         << "\"download_url\": \"/api/download?name=" << json_escape(saved_name) << "\""
         << ", \"thumbnail_url\": \"/images/" << json_escape(saved_name) << "\""
         << ", \"thumbnail_status\": \"" << thumbnail_status << "\""
         << ", \"status_url\": \"/api/image-status?name="
         << json_escape(saved_name) << "\""
         << ", \"delete_url\": \"/api/images?name=" << json_escape(saved_name) << "\""
         << ", \"sha256\": \"" << content_hash << "\""
         << "} }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
}

void HttpResponse::queue_thumbnail_processing(
    const std::string& saved_name,
    const std::string& upload_token
) {
    const std::string thumbnail_name = thumbnail_name_for(saved_name);
    const std::string source_path =
        (std::filesystem::path(images_dir()) / saved_name).string();
    const std::string thumbnail_path =
        (std::filesystem::path(images_dir()) / "thumbnails" /
         thumbnail_name).string();
    const bool thumbnail_queued = ImageProcessor::instance().try_enqueue({
        source_path,
        thumbnail_path,
        [saved_name, upload_token, source_path, thumbnail_path](
            ImageProcessResult result
        ) {
            if (!std::filesystem::is_regular_file(source_path)) {
                std::error_code cleanup_error;
                std::filesystem::remove(thumbnail_path, cleanup_error);
                return;
            }
            const std::string status = result.ok ? "READY" : "FAILED";
            if (!result.ok) {
                Metrics::instance().inc_thumbnail_failed();
                LOG_WARN("thumbnail generation failed for " + saved_name +
                    ": " + result.error);
            }
            else {
                Metrics::instance().inc_thumbnail_processed();
            }
            ImageRedisService::instance().put_image_status_for_generation(
                saved_name,
                status,
                upload_token,
                [](RedisWriteResult result) {
                    if (result.ok && result.applied) {
                        ImageRedisService::instance().invalidate_image_list(
                            [](RedisWriteResult) {});
                    }
                }
            );
        }
    });
    if (!thumbnail_queued) {
        Metrics::instance().inc_rejected_image_processing_task();
        ImageRedisService::instance().put_image_status_for_generation(
            saved_name,
            "FAILED",
            upload_token,
            [](RedisWriteResult result) {
                if (result.ok && result.applied) {
                    ImageRedisService::instance().invalidate_image_list(
                        [](RedisWriteResult) {});
                }
            }
        );
    }
}

std::string HttpResponse::build_delete_image_response(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive
) {
    (void)request;
    (void)session;
    return build_json_response(
        503,
        "Service Unavailable",
        "{ \"ok\": false, \"message\": \"image deletion requires async response path\" }\n",
        keep_alive
    );
}

std::string HttpResponse::build_download_response(
    const HttpRequest& request,
    bool keep_alive
) {
    std::string name = query_value(request.path(), "name");
    name = safe_filename(url_decode(name));

    if (name.empty() || !is_allowed_image_name(name)) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"invalid image name\" }\n",
            keep_alive
        );
    }

    std::string file_path = images_dir() + "/" + name;
    if (!std::filesystem::is_regular_file(file_path)) {
        return build_404_response(keep_alive);
    }

    ImageRedisService::instance().increment_image_download(
        name,
        [](RedisWriteResult result) {
            if (!result.ok) {
                LOG_WARN("increment image download failed: " + result.error);
            }
        }
    );


    if (use_x_accel_redirect()) {
        return build_accel_image_response(name, true, keep_alive);
    }

    return build_file_response(
        file_path,
        keep_alive,
        "Content-Disposition: attachment; filename=\"" + name + "\"\r\n"
    );
}

std::string HttpResponse::build_db_status_response(bool keep_alive) {
    bool available = UserStore::instance().available();
    std::string message = UserStore::instance().status_message();

    std::ostringstream body;
    body << "{ \"ok\": true, "
         << "\"available\": " << (available ? "true" : "false") << ", "
         << "\"message\": \"" << json_escape(message) << "\" }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
}

std::string HttpResponse::build_session_status_response(bool keep_alive) {
    bool available = AsyncSessionManager::instance().available();
    std::string message = AsyncSessionManager::instance().status_message();

    std::ostringstream body;
    body << "{ \"ok\": true, "
         << "\"available\": " << (available ? "true" : "false") << ", "
         << "\"message\": \"" << json_escape(message) << "\" }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
}

std::string HttpResponse::images_dir() const {
    namespace fs = std::filesystem;
    return (fs::path(web_root_) / "images").string();
}

bool HttpResponse::write_uploaded_image(
    const std::string& saved_name,
    const std::string& content,
    std::string& error
) const {
    if (saved_name.empty() || safe_filename(saved_name) != saved_name ||
        !is_allowed_image_name(saved_name)) {
        error = "invalid server-side image name";
        return false;
    }

    namespace fs = std::filesystem;
    fs::path dir(images_dir());

    std::error_code ec;
    fs::create_directories(dir, ec);

    if (ec) {
        error = "create images directory failed";
        return false;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    fs::path file_path = dir / saved_name;
    fs::path temporary_path = dir /
        ("." + saved_name + "." + std::to_string(static_cast<long long>(now)) + ".uploading");

    if (fs::exists(file_path, ec)) {
        error = "image file already exists";
        return false;
    }

    std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);

    if (!file.is_open()) {
        error = "open upload file failed";
        return false;
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));

    if (!file.good()) {
        file.close();
        fs::remove(temporary_path, ec);
        error = "write upload file failed";
        return false;
    }

    file.close();
    fs::rename(temporary_path, file_path, ec);

    if (ec) {
        fs::remove(temporary_path, ec);
        error = "publish upload file failed";
        return false;
    }

    return true;
}

std::string HttpResponse::build_404_response(bool keep_alive) {
    LRUCache::Value body;

    std::string file_path = web_root_ + "/404.html";

    if (!read_file_with_cache(file_path, body)) {
        body = std::make_shared<const std::string>(
            "<html>"
            "<body>"
            "<h1>404 Not Found</h1>"
            "</body>"
            "</html>"
        );
    }

    return build_raw_response(
        404,
        "Not Found",
        "text/html; charset=utf-8",
        *body,
        keep_alive
    );
}

std::string HttpResponse::build_405_response(bool keep_alive) {
    std::string body =
        "<html>"
        "<body>"
        "<h1>405 Method Not Allowed</h1>"
        "<p>Only GET is supported now.</p>"
        "</body>"
        "</html>";

    return build_raw_response(
        405,
        "Method Not Allowed",
        "text/html; charset=utf-8",
        body,
        keep_alive
    );
}

std::string HttpResponse::build_json_response(
    int status_code,
    const std::string& status_text,
    const std::string& body,
    bool keep_alive
) {
    return build_raw_response(
        status_code,
        status_text,
        "application/json; charset=utf-8",
        body,
        keep_alive
    );
}

std::string HttpResponse::build_raw_response(
    int status_code,
    const std::string& status_text,
    const std::string& content_type,
    const std::string& body,
    bool keep_alive
) {
    std::string response;

    response += "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n";
    response += "\r\n";
    response += body;

    return response;
}

std::string HttpResponse::build_response_headers(
    int status_code,
    const std::string& status_text,
    const std::string& content_type,
    uint64_t content_length,
    bool keep_alive,
    const std::string& extra_headers
) const {
    std::string response;
    response += "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(content_length) + "\r\n";
    response += "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n";
    response += extra_headers;
    response += "\r\n";
    return response;
}

std::string HttpResponse::build_raw_response(
    int status_code,
    const std::string& status_text,
    const std::string& content_type,
    const std::string& body,
    bool keep_alive,
    const std::string& extra_headers
) {
    std::string response;

    response += "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n";
    response += extra_headers;
    response += "\r\n";
    response += body;

    return response;
}

std::string HttpResponse::get_content_type(const std::string& file_path) const {
    if (has_suffix(file_path, ".html")) {
        return "text/html; charset=utf-8";
    }

    if (has_suffix(file_path, ".css")) {
        return "text/css; charset=utf-8";
    }

    if (has_suffix(file_path, ".js")) {
        return "application/javascript; charset=utf-8";
    }

    if (has_suffix(file_path, ".png")) {
        return "image/png";
    }

    if (has_suffix(file_path, ".jpg") || has_suffix(file_path, ".jpeg")) {
        return "image/jpeg";
    }

    if (has_suffix(file_path, ".gif")) {
        return "image/gif";
    }

    if (has_suffix(file_path, ".webp")) {
        return "image/webp";
    }

    if (has_suffix(file_path, ".svg")) {
        return "image/svg+xml";
    }

    if (has_suffix(file_path, ".ico")) {
        return "image/x-icon";
    }

    if (has_suffix(file_path, ".txt")) {
        return "text/plain; charset=utf-8";
    }

    return "application/octet-stream";
}

bool HttpResponse::read_file_with_cache(
    const std::string& file_path,
    LRUCache::Value& content
) {
    if (file_cache_.get(file_path, content)) {
        Metrics::instance().inc_cache_hit();
        return true;
    }

    Metrics::instance().inc_cache_miss();

    std::ifstream file(file_path, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    std::ostringstream buffer;

    buffer << file.rdbuf();

    content = std::make_shared<const std::string>(buffer.str());

    file_cache_.put(file_path, content);

    return true;
}

std::string HttpResponse::normalize_path(const std::string& request_path) const {
    if (request_path.empty()) {
        return "";
    }

    std::string path = request_path;

    size_t query_pos = path.find('?');

    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    if (path.find("..") != std::string::npos) {
        return "";
    }

    if (path == "/") {
        path = "/index.html";
    }

    if (path.front() != '/') {
        path = "/" + path;
    }

    return path;
}

bool HttpResponse::has_suffix(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) {
        return false;
    }

    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HttpResponse::starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
        str.compare(0, prefix.size(), prefix) == 0;
}

std::string HttpResponse::path_without_query(const std::string& path) {
    size_t query_pos = path.find('?');
    return query_pos == std::string::npos ? path : path.substr(0, query_pos);
}

std::string HttpResponse::json_escape(const std::string& value) {
    std::ostringstream oss;

    for (char ch : value) {
        switch (ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << ch;
            break;
        }
    }

    return oss.str();
}

std::string HttpResponse::url_decode(const std::string& value) {
    std::string result;

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            result.push_back(' ');
            continue;
        }

        if (value[i] == '%' && i + 2 < value.size()) {
            std::string hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            result.push_back(decoded);
            i += 2;
            continue;
        }

        result.push_back(value[i]);
    }

    return result;
}

std::unordered_map<std::string, std::string> HttpResponse::parse_urlencoded(
    const std::string& body
) {
    std::unordered_map<std::string, std::string> result;
    size_t start = 0;

    while (start <= body.size()) {
        size_t end = body.find('&', start);

        if (end == std::string::npos) {
            end = body.size();
        }

        std::string pair = body.substr(start, end - start);
        size_t equal_pos = pair.find('=');

        if (equal_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, equal_pos));
            std::string value = url_decode(pair.substr(equal_pos + 1));
            result[key] = value;
        }

        if (end == body.size()) {
            break;
        }

        start = end + 1;
    }

    return result;
}

bool HttpResponse::parse_multipart_image(
    const HttpRequest& request,
    std::string& filename,
    std::string& content,
    std::string& error
) {
    std::string content_type = request.get_header("content-type");
    std::string boundary_key = "boundary=";
    size_t boundary_pos = content_type.find(boundary_key);

    if (boundary_pos == std::string::npos) {
        error = "missing multipart boundary";
        return false;
    }

    std::string boundary = "--" + content_type.substr(boundary_pos + boundary_key.size());
    const std::string& body = request.body();

    size_t part_start = body.find(boundary);

    if (part_start == std::string::npos) {
        error = "multipart boundary not found";
        return false;
    }

    part_start += boundary.size();
    if (body.compare(part_start, 2, "\r\n") == 0) {
        part_start += 2;
    }

    size_t header_end = body.find("\r\n\r\n", part_start);

    if (header_end == std::string::npos) {
        error = "multipart headers not complete";
        return false;
    }

    std::string part_headers = body.substr(part_start, header_end - part_start);
    std::istringstream stream(part_headers);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string lower = line;
        std::transform(
            lower.begin(),
            lower.end(),
            lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
        );

        if (lower.find("content-disposition:") == 0) {
            size_t name_pos = lower.find("filename=");

            if (name_pos != std::string::npos) {
                size_t quote1 = line.find('"', name_pos);
                size_t quote2 = line.find('"', quote1 + 1);

                if (quote1 == std::string::npos || quote2 == std::string::npos) {
                    error = "invalid filename";
                    return false;
                }

                filename = line.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
    }

    if (filename.empty()) {
        error = "filename not found";
        return false;
    }

    size_t content_start = header_end + 4;
    size_t content_end = body.find(boundary, content_start);

    if (content_end == std::string::npos) {
        error = "multipart ending not found";
        return false;
    }

    if (content_end >= 2 && body.compare(content_end - 2, 2, "\r\n") == 0) {
        content_end -= 2;
    }

    content = body.substr(content_start, content_end - content_start);

    if (content.empty()) {
        error = "empty upload file";
        return false;
    }

    if (content.size() > 6 * 1024 * 1024) {
        error = "image is too large";
        return false;
    }

    return true;
}

bool HttpResponse::is_allowed_image_name(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return has_suffix(lower, ".png") ||
        has_suffix(lower, ".jpg") ||
        has_suffix(lower, ".jpeg") ||
        has_suffix(lower, ".webp") ||
        has_suffix(lower, ".gif");
}

bool HttpResponse::image_content_matches_name(
    const std::string& filename,
    const std::string& content
) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (has_suffix(lower, ".png")) {
        static const unsigned char signature[]{ 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        return content.size() >= sizeof(signature) &&
            std::memcmp(content.data(), signature, sizeof(signature)) == 0;
    }

    if (has_suffix(lower, ".jpg") || has_suffix(lower, ".jpeg")) {
        return content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xff &&
            static_cast<unsigned char>(content[1]) == 0xd8 &&
            static_cast<unsigned char>(content[2]) == 0xff;
    }

    if (has_suffix(lower, ".gif")) {
        return content.size() >= 6 &&
            (content.compare(0, 6, "GIF87a") == 0 ||
             content.compare(0, 6, "GIF89a") == 0);
    }

    if (has_suffix(lower, ".webp")) {
        return content.size() >= 12 &&
            content.compare(0, 4, "RIFF") == 0 &&
            content.compare(8, 4, "WEBP") == 0;
    }

    return false;
}

std::string HttpResponse::safe_filename(const std::string& filename) {
    std::string name = filename;

    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }

    std::string result;
    for (char ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-') {
            result.push_back(ch);
        }
    }

    return result;
}

bool HttpResponse::use_x_accel_redirect() {
    const char* value = std::getenv("X_ACCEL_REDIRECT");

    if (!value) {
        return false;
    }

    std::string enabled = value;

    std::transform(
        enabled.begin(),
        enabled.end(),
        enabled.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return enabled == "1" || enabled == "true" || enabled == "yes";
}

std::string HttpResponse::query_value(const std::string& path, const std::string& name) {
    size_t q = path.find('?');

    if (q == std::string::npos) {
        return "";
    }

    std::string query = path.substr(q + 1);
    size_t start = 0;

    while (start <= query.size()) {
        size_t end = query.find('&', start);

        if (end == std::string::npos) {
            end = query.size();
        }

        std::string pair = query.substr(start, end - start);
        size_t eq = pair.find('=');

        if (eq != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq));
            if (key == name) {
                return pair.substr(eq + 1);
            }
        }

        if (end == query.size()) {
            break;
        }
        start = end + 1;
    }

    return "";
}


std::string HttpResponse::without_body(const std::string& response) {
    size_t body_pos = response.find("\r\n\r\n");

    if (body_pos == std::string::npos) {
        return response;
    }

    return response.substr(0, body_pos + 4);
}

std::string HttpResponse::cookie_value(
    const HttpRequest& request,
    const std::string& name
) {
    std::string cookie = request.get_header("cookie");
    size_t start = 0;

    while (start < cookie.size()) {
        while (start < cookie.size() && (cookie[start] == ' ' || cookie[start] == ';')) {
            ++start;
        }

        size_t end = cookie.find(';', start);

        if (end == std::string::npos) {
            end = cookie.size();
        }

        std::string pair = cookie.substr(start, end - start);
        size_t equal_pos = pair.find('=');

        if (equal_pos != std::string::npos) {
            std::string key = pair.substr(0, equal_pos);

            if (key == name) {
                return pair.substr(equal_pos + 1);
            }
        }

        start = end + 1;
    }

    return "";
}

bool HttpResponse::requires_session(const HttpRequest& request) {
    std::string clean_path = path_without_query(request.path());

    if (starts_with(clean_path, "/images/") ||
        starts_with(clean_path, "/thumbnails/")) {
        return true;
    }

    if (clean_path == "/api/me" || clean_path == "/api/images" ||
        clean_path == "/api/image-status" || clean_path == "/api/rankings" ||
        clean_path == "/api/upload-image" || clean_path == "/api/download" ||
        clean_path == "/api/image-view") {
        return true;
    }

    return request.method() == "DELETE" && clean_path == "/api/images";
}

bool HttpResponse::request_cancelled() const {
    return cancelled_ && cancelled_->load(std::memory_order_acquire);
}
