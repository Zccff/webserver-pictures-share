#include "http/HttpResponse.h"

#include "auth/SessionManager.h"
#include "db/UserStore.h"
#include "utils/Metrics.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr uintmax_t MAX_MEMORY_CACHED_FILE_SIZE = 256 * 1024;

std::mutex g_images_cache_mutex;
std::string g_images_cache_body;
std::chrono::steady_clock::time_point g_images_cache_expires_at;

int image_list_cache_ttl_seconds() {
    const char* value = std::getenv("IMAGE_LIST_CACHE_TTL_SECONDS");

    if (!value || value[0] == '\0') {
        return 1;
    }

    int parsed = std::atoi(value);
    return parsed < 0 ? 0 : parsed;
}

void invalidate_image_list_cache() {
    std::lock_guard<std::mutex> lock(g_images_cache_mutex);
    g_images_cache_body.clear();
    g_images_cache_expires_at = std::chrono::steady_clock::time_point{};
}

}

HttpResponse::HttpResponse(
    std::string web_root,
    LRUCache& file_cache,
    size_t thread_pool_queue_size
)
    : web_root_(std::move(web_root)),
    file_cache_(file_cache),
    thread_pool_queue_size_(thread_pool_queue_size) {
}

std::string HttpResponse::build_response(
    const HttpRequest& request,
    bool keep_alive
) {
    std::string clean_path = path_without_query(request.path());

    if (starts_with(clean_path, "/api/")) {
        return build_api_response(request, keep_alive);
    }

    if (starts_with(clean_path, "/images/")) {
        if (!require_session(request)) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_protected_image_response(clean_path, keep_alive);
    }

    return build_response(request.method(), request.path(), keep_alive);
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

    if (use_x_accel_redirect()) {
        return build_accel_image_response(name, false, keep_alive);
    }

    return build_static_response("/images/" + name, keep_alive);
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
    std::string clean_path = path_without_query(request.path());

    if (request.method() == "POST" && clean_path == "/api/register") {
        return build_register_response(request, keep_alive);
    }

    if (request.method() == "POST" && clean_path == "/api/login") {
        return build_login_response(request, keep_alive);
    }

    if (request.method() == "POST" && clean_path == "/api/logout") {
        return build_logout_response(request, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/me") {
        return build_me_response(request, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/images") {
        if (!require_session(request)) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_images_response(keep_alive);
    }

    if (request.method() == "POST" && clean_path == "/api/upload-image") {
        if (!require_session(request)) {
            return build_json_response(
                401,
                "Unauthorized",
                "{ \"ok\": false, \"message\": \"login required\" }\n",
                keep_alive
            );
        }

        return build_upload_image_response(request, keep_alive);
    }

    if (request.method() == "GET" && clean_path == "/api/download") {
        if (!require_session(request)) {
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

    if (request.method() != "GET" && request.method() != "POST") {
        return build_405_response(keep_alive);
    }

    return build_json_response(
        404,
        "Not Found",
        "{ \"ok\": false, \"message\": \"api endpoint not found\" }\n",
        keep_alive
    );
}

std::string HttpResponse::build_register_response(
    const HttpRequest& request,
    bool keep_alive
) {
    auto form = parse_urlencoded(request.body());

    RegisterResult result = UserStore::instance().register_user(
        form["username"],
        form["password"],
        form["display_name"],
        form["email"]
    );

    std::ostringstream body;
    body << "{ \"ok\": " << (result.ok ? "true" : "false")
         << ", \"message\": \"" << json_escape(result.message) << "\" }\n";

    return build_json_response(
        result.ok ? 200 : 400,
        result.ok ? "OK" : "Bad Request",
        body.str(),
        keep_alive
    );
}

std::string HttpResponse::build_login_response(
    const HttpRequest& request,
    bool keep_alive
) {
    auto form = parse_urlencoded(request.body());

    LoginResult result = UserStore::instance().login_user(
        form["username"],
        form["password"]
    );

    std::ostringstream body;
    body << "{ \"ok\": " << (result.ok ? "true" : "false")
         << ", \"message\": \"" << json_escape(result.message) << "\"";

    if (result.ok) {
        std::string token = SessionManager::instance().create_session(
            result.username,
            result.display_name
        );

        if (token.empty()) {
            return build_json_response(
                500,
                "Internal Server Error",
                "{ \"ok\": false, \"message\": \"session store unavailable\" }\n",
                keep_alive
            );
        }

        body << ", \"user\": {"
             << "\"username\": \"" << json_escape(result.username) << "\", "
             << "\"display_name\": \"" << json_escape(result.display_name) << "\""
             << " }";

        body << " }\n";

        return build_raw_response(
            200,
            "OK",
            "application/json; charset=utf-8",
            body.str(),
            keep_alive,
            "Set-Cookie: sid=" + token + "; Path=/; HttpOnly; SameSite=Lax\r\n"
        );
    }

    body << " }\n";

    return build_json_response(
        401,
        "Unauthorized",
        body.str(),
        keep_alive
    );
}

std::string HttpResponse::build_logout_response(
    const HttpRequest& request,
    bool keep_alive
) {
    std::string token = cookie_value(request, "sid");

    if (!token.empty()) {
        SessionManager::instance().destroy_session(token);
    }

    return build_raw_response(
        200,
        "OK",
        "application/json; charset=utf-8",
        "{ \"ok\": true, \"message\": \"logged out\" }\n",
        keep_alive,
        "Set-Cookie: sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax\r\n"
    );
}

std::string HttpResponse::build_me_response(
    const HttpRequest& request,
    bool keep_alive
) {
    SessionInfo session;
    std::string token = cookie_value(request, "sid");

    if (token.empty() || !SessionManager::instance().get_session(token, session)) {
        return build_json_response(
            401,
            "Unauthorized",
            "{ \"ok\": false, \"message\": \"login required\" }\n",
            keep_alive
        );
    }

    std::ostringstream body;
    body << "{ \"ok\": true, \"user\": {"
         << "\"username\": \"" << json_escape(session.username) << "\", "
         << "\"display_name\": \"" << json_escape(session.display_name) << "\""
         << "} }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
}

std::string HttpResponse::build_images_response(bool keep_alive) {
    namespace fs = std::filesystem;

    int cache_ttl = image_list_cache_ttl_seconds();

    if (cache_ttl > 0) {
        std::lock_guard<std::mutex> lock(g_images_cache_mutex);

        if (!g_images_cache_body.empty() &&
            std::chrono::steady_clock::now() < g_images_cache_expires_at) {
            return build_json_response(200, "OK", g_images_cache_body, keep_alive);
        }
    }

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

        body << "{"
             << "\"name\": \"" << json_escape(images[i]) << "\", "
             << "\"url\": \"/images/" << json_escape(images[i]) << "\", "
             << "\"download_url\": \"/api/download?name=" << json_escape(images[i]) << "\""
             << "}";
    }

    body << "] }\n";

    std::string body_text = body.str();

    if (cache_ttl > 0) {
        std::lock_guard<std::mutex> lock(g_images_cache_mutex);
        g_images_cache_body = body_text;
        g_images_cache_expires_at = std::chrono::steady_clock::now() +
            std::chrono::seconds(cache_ttl);
    }

    return build_json_response(200, "OK", body_text, keep_alive);
}

std::string HttpResponse::build_upload_image_response(
    const HttpRequest& request,
    bool keep_alive
) {
    std::string filename;
    std::string content;
    std::string error;

    if (!parse_multipart_image(request, filename, content, error)) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"" + json_escape(error) + "\" }\n",
            keep_alive
        );
    }

    std::string saved_name;

    if (!write_uploaded_image(filename, content, saved_name, error)) {
        return build_json_response(
            400,
            "Bad Request",
            "{ \"ok\": false, \"message\": \"" + json_escape(error) + "\" }\n",
            keep_alive
        );
    }

    invalidate_image_list_cache();

    std::ostringstream body;
    body << "{ \"ok\": true, \"message\": \"uploaded\", "
         << "\"file\": {"
         << "\"name\": \"" << json_escape(saved_name) << "\", "
         << "\"url\": \"/images/" << json_escape(saved_name) << "\", "
         << "\"download_url\": \"/api/download?name=" << json_escape(saved_name) << "\""
         << "} }\n";

    return build_json_response(200, "OK", body.str(), keep_alive);
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
    bool available = SessionManager::instance().available();
    std::string message = SessionManager::instance().status_message();

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
    const std::string& filename,
    const std::string& content,
    std::string& saved_name,
    std::string& error
) const {
    if (!is_allowed_image_name(filename)) {
        error = "only png, jpg, jpeg, webp, gif are allowed";
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

    std::string base_name = safe_filename(filename);

    if (base_name.empty()) {
        error = "invalid filename";
        return false;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    saved_name = std::to_string(static_cast<long long>(now)) + "_" + base_name;

    fs::path file_path = dir / saved_name;
    std::ofstream file(file_path, std::ios::binary);

    if (!file.is_open()) {
        error = "open upload file failed";
        return false;
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));

    if (!file.good()) {
        error = "write upload file failed";
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
    return has_suffix(filename, ".png") ||
        has_suffix(filename, ".jpg") ||
        has_suffix(filename, ".jpeg") ||
        has_suffix(filename, ".webp") ||
        has_suffix(filename, ".gif");
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

bool HttpResponse::require_session(const HttpRequest& request) {
    SessionInfo session;
    std::string token = cookie_value(request, "sid");
    return !token.empty() && SessionManager::instance().get_session(token, session);
}
