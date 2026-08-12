#include "http/HttpConnection.h"

#include "log/AsyncLogger.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr size_t READ_BUFFER_SIZE = 4096;

}

HttpConnection::HttpConnection(int fd, uint64_t generation)
    : fd_(fd), generation_(generation) {
}

HttpConnection::~HttpConnection() {
    if (send_file_fd_ >= 0) {
        close(send_file_fd_);
    }
}

int HttpConnection::fd() const {
    return fd_;
}

uint64_t HttpConnection::generation() const {
    return generation_;
}

HttpConnection::ReadStatus HttpConnection::read_request(HttpRequest& request) {
    char buffer[READ_BUFFER_SIZE];

    while (true) {
        ssize_t bytes_read = recv(fd_, buffer, sizeof(buffer), 0);

        if (bytes_read > 0) {
            read_buffer_.append(buffer, static_cast<size_t>(bytes_read));

            if (read_buffer_.size() > MAX_REQUEST_SIZE) {
                LOG_WARN("request too large, fd=" + std::to_string(fd_));
                return ReadStatus::BadRequest;
            }

            continue;
        }

        if (bytes_read == 0) {
            return ReadStatus::Closed;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR(
                "recv failed, fd=" + std::to_string(fd_) +
                ", reason=" + std::strerror(errno)
            );
            return ReadStatus::Error;
        }

        break;
    }

    size_t request_size = 0;

    if (!complete_request_size(request_size)) {
        return ReadStatus::Incomplete;
    }

    if (request_size > MAX_REQUEST_SIZE) {
        return ReadStatus::BadRequest;
    }

    std::string raw_request = read_buffer_.substr(0, request_size);
    read_buffer_.erase(0, request_size);

    if (!request.parse(raw_request)) {
        return ReadStatus::BadRequest;
    }

    return ReadStatus::RequestReady;
}

void HttpConnection::begin_response(
    std::string response,
    std::string file_path,
    uint64_t file_size,
    bool keep_alive
) {
    reset_response();
    write_buffer_ = std::move(response);
    send_file_path_ = std::move(file_path);
    send_file_size_ = file_size;
    send_file_remaining_ = file_size;
    keep_alive_after_response_ = keep_alive;
}

HttpConnection::WriteStatus HttpConnection::flush_response() {
    while (write_offset_ < write_buffer_.size()) {
        ssize_t bytes_sent = send(
            fd_,
            write_buffer_.data() + write_offset_,
            write_buffer_.size() - write_offset_,
            MSG_NOSIGNAL
        );

        if (bytes_sent > 0) {
            write_offset_ += static_cast<size_t>(bytes_sent);
            continue;
        }

        if (bytes_sent < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return WriteStatus::WouldBlock;
        }

        return WriteStatus::Error;
    }

    if (send_file_remaining_ > 0 && send_file_fd_ < 0) {
        send_file_fd_ = open(send_file_path_.c_str(), O_RDONLY | O_CLOEXEC);

        if (send_file_fd_ < 0) {
            LOG_ERROR(
                "open for sendfile failed, path=" + send_file_path_ +
                ", reason=" + std::strerror(errno)
            );
            return WriteStatus::Error;
        }
    }

    while (send_file_remaining_ > 0) {
        constexpr ssize_t MAX_CHUNK = std::numeric_limits<ssize_t>::max();
        size_t chunk = send_file_remaining_ > static_cast<uint64_t>(MAX_CHUNK)
            ? static_cast<size_t>(MAX_CHUNK)
            : static_cast<size_t>(send_file_remaining_);

        ssize_t bytes_sent = sendfile(
            fd_,
            send_file_fd_,
            &send_file_offset_,
            chunk
        );

        if (bytes_sent > 0) {
            send_file_remaining_ -= static_cast<uint64_t>(bytes_sent);
            continue;
        }

        if (bytes_sent < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return WriteStatus::WouldBlock;
        }

        return WriteStatus::Error;
    }

    reset_response();
    return WriteStatus::Complete;
}

bool HttpConnection::keep_alive_after_response() const {
    return keep_alive_after_response_;
}

bool HttpConnection::complete_request_size(size_t& request_size) const {
    size_t header_end = read_buffer_.find("\r\n\r\n");

    if (header_end == std::string::npos) {
        return false;
    }

    std::string headers = read_buffer_.substr(0, header_end);
    std::istringstream stream(headers);
    std::string line;
    size_t content_length = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon_pos = line.find(':');

        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colon_pos);

        for (char& ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (key != "content-length") {
            continue;
        }

        std::string value = line.substr(colon_pos + 1);
        char* end = nullptr;
        errno = 0;
        unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);

        while (end && *end != '\0' &&
            std::isspace(static_cast<unsigned char>(*end))) {
            ++end;
        }

        if (end == value.c_str() || (end && *end != '\0') ||
            errno == ERANGE || parsed > MAX_REQUEST_SIZE) {
            request_size = MAX_REQUEST_SIZE + 1;
            return true;
        }

        content_length = static_cast<size_t>(parsed);
        break;
    }

    if (content_length > MAX_REQUEST_SIZE ||
        header_end + 4 > MAX_REQUEST_SIZE - content_length) {
        request_size = MAX_REQUEST_SIZE + 1;
        return true;
    }

    request_size = header_end + 4 + content_length;
    return read_buffer_.size() >= request_size;
}

void HttpConnection::reset_response() {
    if (send_file_fd_ >= 0) {
        close(send_file_fd_);
        send_file_fd_ = -1;
    }

    write_buffer_.clear();
    write_offset_ = 0;
    send_file_path_.clear();
    send_file_size_ = 0;
    send_file_remaining_ = 0;
    send_file_offset_ = 0;
}
