#pragma once

#include "http/HttpRequest.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <sys/types.h>

class HttpConnection {
public:
    enum class ReadStatus {
        Incomplete,
        RequestReady,
        BadRequest,
        Closed,
        Error
    };

    enum class WriteStatus {
        Complete,
        WouldBlock,
        Error
    };

    HttpConnection(
        int fd,
        uint64_t generation,
        std::string upload_temp_dir = ""
    );

    ~HttpConnection();

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    int fd() const;

    uint64_t generation() const;

    std::shared_ptr<std::atomic<bool>> cancellation_token() const;

    void cancel_work();

    ReadStatus read_request(HttpRequest& request);

    void begin_response(
        std::string response,
        std::string file_path,
        uint64_t file_size,
        bool keep_alive
    );

    WriteStatus flush_response();

    bool keep_alive_after_response() const;

private:
    bool complete_request_size(size_t& request_size) const;

    void reset_response();

    ReadStatus start_streamed_upload();

    ReadStatus append_streamed_upload(const char* data, size_t size);


    ReadStatus read_request_buffered(HttpRequest& request);
    ReadStatus finish_streamed_upload(HttpRequest& request);

    void reset_streamed_upload(bool remove_file);

private:
    static constexpr size_t MAX_REQUEST_SIZE = 8 * 1024 * 1024;

    static constexpr uint64_t MAX_STREAMED_UPLOAD_SIZE = 6 * 1024 * 1024;
    int fd_;
    uint64_t generation_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::string read_buffer_;

    std::string upload_temp_dir_;
    std::string upload_headers_;
    std::string upload_original_name_;
    std::string upload_temp_path_;
    uint64_t upload_expected_{ 0 };
    uint64_t upload_written_{ 0 };
    int upload_fd_{ -1 };
    bool upload_active_{ false };
    std::string write_buffer_;
    size_t write_offset_{ 0 };

    std::string send_file_path_;
    int send_file_fd_{ -1 };
    uint64_t send_file_size_{ 0 };
    uint64_t send_file_remaining_{ 0 };
    off_t send_file_offset_{ 0 };
    bool keep_alive_after_response_{ false };
};
