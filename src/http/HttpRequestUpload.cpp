#include "http/HttpRequest.h"

#include <filesystem>
#include <utility>

HttpRequest::StreamedUpload::~StreamedUpload() {
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
}

bool HttpRequest::has_streamed_upload() const {
    return static_cast<bool>(streamed_upload_);
}

const std::string& HttpRequest::upload_temp_path() const {
    static const std::string empty;
    return streamed_upload_ ? streamed_upload_->temporary_path : empty;
}

const std::string& HttpRequest::upload_original_name() const {
    static const std::string empty;
    return streamed_upload_ ? streamed_upload_->original_name : empty;
}

uint64_t HttpRequest::upload_size() const {
    return streamed_upload_ ? streamed_upload_->size : 0;
}

void HttpRequest::attach_streamed_upload(
    std::string temporary_path,
    std::string original_name,
    uint64_t size
) {
    streamed_upload_ = std::make_shared<StreamedUpload>();
    streamed_upload_->temporary_path = std::move(temporary_path);
    streamed_upload_->original_name = std::move(original_name);
    streamed_upload_->size = size;
}
