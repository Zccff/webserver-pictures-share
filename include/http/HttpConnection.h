#pragma once

#include "http/HttpRequest.h"

#include <cstddef>
#include <cstdint>
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

    HttpConnection(int fd, uint64_t generation);

    ~HttpConnection();

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    int fd() const;

    uint64_t generation() const;

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

private:
    static constexpr size_t MAX_REQUEST_SIZE = 8 * 1024 * 1024;

    int fd_;
    uint64_t generation_;
    std::string read_buffer_;

    std::string write_buffer_;
    size_t write_offset_{ 0 };

    std::string send_file_path_;
    int send_file_fd_{ -1 };
    uint64_t send_file_size_{ 0 };
    uint64_t send_file_remaining_{ 0 };
    off_t send_file_offset_{ 0 };
    bool keep_alive_after_response_{ false };
};
