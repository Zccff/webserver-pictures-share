#include "http/HttpConnection.h"

#include "log/AsyncLogger.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t UPLOAD_READ_BUFFER_SIZE = 16 * 1024;

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

bool parse_length(const std::string& value, uint64_t& length) {
    if (value.empty() || value.front() == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    while (end && *end != '\0' &&
           std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (end == value.c_str() || (end && *end != '\0') || errno == ERANGE) {
        return false;
    }
    length = static_cast<uint64_t>(parsed);
    return true;
}

bool has_well_formed_session_cookie(const std::string& cookie) {
    size_t begin = 0;
    while (begin < cookie.size()) {
        size_t end = cookie.find(';', begin);
        if (end == std::string::npos) {
            end = cookie.size();
        }
        while (begin < end && std::isspace(
                static_cast<unsigned char>(cookie[begin]))) {
            ++begin;
        }
        const std::string entry = cookie.substr(begin, end - begin);
        if (entry.rfind("sid=", 0) == 0) {
            const std::string token = entry.substr(4);
            if (token.size() != 64) {
                return false;
            }
            return std::all_of(token.begin(), token.end(), [](char ch) {
                return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
            });
        }
        begin = end + 1;
    }
    return false;
}

}  // namespace

HttpConnection::ReadStatus HttpConnection::read_request(HttpRequest& request) {
    char buffer[UPLOAD_READ_BUFFER_SIZE];

    while (true) {
        if (upload_active_ && upload_written_ == upload_expected_) {
            return finish_streamed_upload(request);
        }

        if (!upload_active_) {
            const size_t header_end = read_buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                ReadStatus status = start_streamed_upload();
                if (status == ReadStatus::BadRequest ||
                    status == ReadStatus::Error) {
                    return status;
                }
                if (!upload_active_) {
                    return read_request_buffered(request);
                }
                if (upload_written_ == upload_expected_) {
                    return finish_streamed_upload(request);
                }
            }
        }

        const ssize_t bytes_read = recv(fd_, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            if (upload_active_) {
                ReadStatus status = append_streamed_upload(
                    buffer,
                    static_cast<size_t>(bytes_read)
                );
                if (status == ReadStatus::RequestReady) {
                    return finish_streamed_upload(request);
                }
                if (status == ReadStatus::BadRequest ||
                    status == ReadStatus::Error) {
                    return status;
                }
            }
            else {
                read_buffer_.append(buffer, static_cast<size_t>(bytes_read));
                if (read_buffer_.size() > MAX_REQUEST_SIZE) {
                    LOG_WARN("request headers too large, fd=" +
                        std::to_string(fd_));
                    return ReadStatus::BadRequest;
                }
            }
            continue;
        }

        if (bytes_read == 0) {
            reset_streamed_upload(true);
            return ReadStatus::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReadStatus::Incomplete;
        }

        LOG_ERROR(
            "recv upload failed, fd=" + std::to_string(fd_) +
            ", reason=" + std::strerror(errno)
        );
        reset_streamed_upload(true);
        return ReadStatus::Error;
    }
}

HttpConnection::ReadStatus HttpConnection::start_streamed_upload() {
    const size_t header_end = read_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return ReadStatus::Incomplete;
    }

    const size_t header_size = header_end + 4;
    const std::string headers = read_buffer_.substr(0, header_size);
    HttpRequest request_headers;
    if (!request_headers.parse(headers)) {
        return ReadStatus::BadRequest;
    }

    const std::string content_type = lower_copy(
        request_headers.get_header("content-type")
    );
    const std::string original_name =
        request_headers.get_header("x-file-name");
    const bool direct_upload =
        request_headers.method() == "POST" &&
        request_headers.path() == "/api/upload-image" &&
        content_type.rfind("image/", 0) == 0 &&
        !original_name.empty() &&
        has_well_formed_session_cookie(request_headers.get_header("cookie"));
    if (!direct_upload) {
        if (request_headers.method() == "POST" &&
            request_headers.path() == "/api/upload-image" &&
            content_type.rfind("image/", 0) == 0 &&
            !original_name.empty()) {
            return ReadStatus::BadRequest;
        }
        return ReadStatus::Incomplete;
    }

    uint64_t expected = 0;
    if (!parse_length(request_headers.get_header("content-length"), expected) ||
        expected == 0 || expected > MAX_STREAMED_UPLOAD_SIZE ||
        original_name.size() > 1024) {
        return ReadStatus::BadRequest;
    }

    std::error_code error;
    std::filesystem::create_directories(upload_temp_dir_, error);
    if (error) {
        LOG_ERROR("create upload temp directory failed: " + error.message());
        return ReadStatus::Error;
    }

    std::string path_template =
        (std::filesystem::path(upload_temp_dir_) / "upload-XXXXXX").string();
    std::vector<char> mutable_path(path_template.begin(), path_template.end());
    mutable_path.push_back('\0');
    upload_fd_ = mkstemp(mutable_path.data());
    if (upload_fd_ < 0) {
        LOG_ERROR("create upload temp file failed: " +
            std::string(std::strerror(errno)));
        return ReadStatus::Error;
    }
    fcntl(upload_fd_, F_SETFD, FD_CLOEXEC);

    upload_active_ = true;
    upload_headers_ = headers;
    upload_original_name_ = original_name;
    upload_temp_path_ = mutable_path.data();
    upload_expected_ = expected;
    upload_written_ = 0;

    std::string already_received = read_buffer_.substr(header_size);
    read_buffer_.clear();
    return append_streamed_upload(
        already_received.data(),
        already_received.size()
    );
}

HttpConnection::ReadStatus HttpConnection::append_streamed_upload(
    const char* data,
    size_t size
) {
    if (!upload_active_ || upload_fd_ < 0 ||
        upload_written_ > upload_expected_) {
        reset_streamed_upload(true);
        return ReadStatus::Error;
    }

    const uint64_t remaining = upload_expected_ - upload_written_;
    const size_t file_bytes = static_cast<size_t>(std::min<uint64_t>(
        remaining,
        static_cast<uint64_t>(size)
    ));
    size_t offset = 0;
    while (offset < file_bytes) {
        const ssize_t written = write(
            upload_fd_,
            data + offset,
            file_bytes - offset
        );
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        LOG_ERROR("write upload temp file failed: " +
            std::string(std::strerror(errno)));
        reset_streamed_upload(true);
        return ReadStatus::Error;
    }
    upload_written_ += static_cast<uint64_t>(file_bytes);

    if (size > file_bytes) {
        read_buffer_.append(data + file_bytes, size - file_bytes);
        if (read_buffer_.size() > MAX_REQUEST_SIZE) {
            reset_streamed_upload(true);
            return ReadStatus::BadRequest;
        }
    }
    return upload_written_ == upload_expected_
        ? ReadStatus::RequestReady
        : ReadStatus::Incomplete;
}

HttpConnection::ReadStatus HttpConnection::finish_streamed_upload(
    HttpRequest& request
) {
    if (!upload_active_ || upload_written_ != upload_expected_) {
        return ReadStatus::Incomplete;
    }
    if (upload_fd_ >= 0) {
        close(upload_fd_);
        upload_fd_ = -1;
    }

    const std::string headers = upload_headers_;
    const std::string temporary_path = upload_temp_path_;
    const std::string original_name = upload_original_name_;
    const uint64_t size = upload_written_;
    reset_streamed_upload(false);

    if (!request.parse(headers)) {
        std::error_code error;
        std::filesystem::remove(temporary_path, error);
        return ReadStatus::BadRequest;
    }
    request.attach_streamed_upload(temporary_path, original_name, size);
    return ReadStatus::RequestReady;
}

void HttpConnection::reset_streamed_upload(bool remove_file) {
    if (upload_fd_ >= 0) {
        close(upload_fd_);
        upload_fd_ = -1;
    }
    if (remove_file && !upload_temp_path_.empty()) {
        std::error_code error;
        std::filesystem::remove(upload_temp_path_, error);
    }
    upload_active_ = false;
    upload_headers_.clear();
    upload_original_name_.clear();
    upload_temp_path_.clear();
    upload_expected_ = 0;
    upload_written_ = 0;
}
