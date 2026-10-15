#include "http/HttpRedisPolicy.h"

#include <algorithm>
#include <cctype>

bool is_valid_upload_idempotency_key(const std::string& key) {
    if (key.empty()) {
        return true;
    }
    if (key.size() < 8 || key.size() > 128) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
}

std::string http_response_body(const std::string& response) {
    size_t separator = response.find("\r\n\r\n");
    if (separator == std::string::npos) {
        return "";
    }
    return response.substr(separator + 4);
}
