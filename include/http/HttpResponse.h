#pragma once

#include "cache/LRUCache.h"
#include "http/HttpRequest.h"
#include "auth/AsyncSessionManager.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class HttpResponse {
public:
    using CompletionCallback = std::function<void(std::string response)>;
    using Executor = std::function<bool(std::function<void()> continuation)>;

    HttpResponse(
        std::string web_root,
        LRUCache& file_cache,
        size_t thread_pool_queue_size,
        std::shared_ptr<std::atomic<bool>> cancelled
    );

    std::string build_response(
        const HttpRequest& request,
        bool keep_alive
    );

    void build_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
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
    class UploadRedisFlow;
    class DeleteRedisFlow;

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

    std::string build_protected_thumbnail_response(
        const std::string& request_path,
        bool keep_alive
    );

    std::string build_accel_image_response(
        const std::string& name,
        bool download,
        bool keep_alive
    );

    std::string build_accel_thumbnail_response(
        const std::string& name,
        bool keep_alive
    );

    std::string build_metrics_response(bool keep_alive);

    std::string build_api_response(const HttpRequest& request, bool keep_alive);

    std::string build_api_response(
        const HttpRequest& request,
        const SessionInfo* session,
        bool keep_alive
    );

    std::string build_authenticated_response(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive
    );

    void build_authenticated_response_async(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    void build_rankings_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback
    );

    void build_record_image_view_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback
    );

    void build_images_response_async(
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    void build_image_status_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback
    );

    void build_upload_image_response_async(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    void build_delete_image_response_async(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    std::string build_register_response(const HttpRequest& request, bool keep_alive);

    std::string build_login_response(const HttpRequest& request, bool keep_alive);

    void build_login_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    std::string build_logout_response(const HttpRequest& request, bool keep_alive);

    void build_logout_response_async(
        const HttpRequest& request,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    );

    std::string build_me_response(const SessionInfo& session, bool keep_alive);

    std::string build_images_response(bool keep_alive);

    std::string build_upload_image_response(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive,
        const std::string& precomputed_hash = ""
    );

    void queue_thumbnail_processing(
        const std::string& saved_name,
        const std::string& upload_token
    );

    std::string build_delete_image_response(
        const HttpRequest& request,
        const SessionInfo& session,
        bool keep_alive
    );

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
        const std::string& saved_name,
        const std::string& content,
        std::string& error
    ) const;

    bool publish_streamed_upload(
        const std::string& saved_name,
        const std::string& temporary_path,
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

    static bool image_content_matches_name(
        const std::string& filename,
        const std::string& content
    );

    static bool image_file_matches_name(
        const std::string& filename,
        const std::string& path
    );

    static std::string safe_filename(const std::string& filename);

    static bool use_x_accel_redirect();

    static std::string without_body(const std::string& response);

    static std::string cookie_value(const HttpRequest& request, const std::string& name);

    static bool requires_session(const HttpRequest& request);

    bool request_cancelled() const;

private:
    std::string web_root_;
    LRUCache& file_cache_;
    size_t thread_pool_queue_size_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::string send_file_path_;
    uint64_t send_file_size_{ 0 };
};
