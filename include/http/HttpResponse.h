#pragma once

#include "cache/LRUCache.h"
#include "http/HttpRequest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

class HttpResponse {
public:
    HttpResponse(
        std::string web_root,
        LRUCache& file_cache,
        size_t thread_pool_queue_size
    );

    std::string build_response(
        const HttpRequest& request,
        bool keep_alive
    );

    std::string build_response(
        const std::string& method,
        const std::string& request_path,
        bool keep_alive
    );

    bool has_send_file() const;

    const std::string& send_file_path() const;

    uint64_t send_file_size() const;

private:
    std::string build_static_response(const std::string& request_path, bool keep_alive);

    std::string build_file_response(
        const std::string& file_path,
        bool keep_alive,
        const std::string& extra_headers = ""
    );

    std::string build_protected_image_response(
        const std::string& request_path,
        bool keep_alive
    );

    std::string build_accel_image_response(
        const std::string& name,
        bool download,
        bool keep_alive
    );

    std::string build_metrics_response(bool keep_alive);

    std::string build_api_response(const HttpRequest& request, bool keep_alive);

    std::string build_register_response(const HttpRequest& request, bool keep_alive);

    std::string build_login_response(const HttpRequest& request, bool keep_alive);

    std::string build_logout_response(const HttpRequest& request, bool keep_alive);

    std::string build_me_response(const HttpRequest& request, bool keep_alive);

    std::string build_images_response(bool keep_alive);

    std::string build_upload_image_response(const HttpRequest& request, bool keep_alive);

    std::string build_download_response(const HttpRequest& request, bool keep_alive);

    std::string build_db_status_response(bool keep_alive);

    std::string build_session_status_response(bool keep_alive);

    std::string build_404_response(bool keep_alive);

    std::string build_405_response(bool keep_alive);

    std::string build_json_response(
        int status_code,
        const std::string& status_text,
        const std::string& body,
        bool keep_alive
    );

    std::string build_raw_response(
        int status_code,
        const std::string& status_text,
        const std::string& content_type,
        const std::string& body,
        bool keep_alive,
        const std::string& extra_headers
    );

    std::string build_raw_response(
        int status_code,
        const std::string& status_text,
        const std::string& content_type,
        const std::string& body,
        bool keep_alive
    );

    std::string get_content_type(const std::string& file_path) const;

    bool read_file_with_cache(const std::string& file_path, LRUCache::Value& content);

    std::string build_response_headers(
        int status_code,
        const std::string& status_text,
        const std::string& content_type,
        uint64_t content_length,
        bool keep_alive,
        const std::string& extra_headers
    ) const;

    std::string normalize_path(const std::string& request_path) const;

    std::string images_dir() const;

    bool write_uploaded_image(
        const std::string& filename,
        const std::string& content,
        std::string& saved_name,
        std::string& error
    ) const;

    static bool has_suffix(const std::string& str, const std::string& suffix);

    static bool starts_with(const std::string& str, const std::string& prefix);

    static std::string path_without_query(const std::string& path);

    static std::string json_escape(const std::string& value);

    static std::string url_decode(const std::string& value);

    static std::string query_value(const std::string& path, const std::string& name);

    static std::unordered_map<std::string, std::string> parse_urlencoded(
        const std::string& body
    );

    static bool parse_multipart_image(
        const HttpRequest& request,
        std::string& filename,
        std::string& content,
        std::string& error
    );

    static bool is_allowed_image_name(const std::string& filename);

    static std::string safe_filename(const std::string& filename);

    static bool use_x_accel_redirect();

    static std::string without_body(const std::string& response);

    static std::string cookie_value(const HttpRequest& request, const std::string& name);

    static bool require_session(const HttpRequest& request);

private:
    std::string web_root_;
    LRUCache& file_cache_;
    size_t thread_pool_queue_size_;
    std::string send_file_path_;
    uint64_t send_file_size_{ 0 };
};
