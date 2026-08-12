#include "http/HttpRequest.h"

#include <algorithm>
#include <cctype>
#include <sstream>

bool HttpRequest::parse(const std::string& raw_request) {
    method_.clear();
    path_.clear();
    version_.clear();
    body_.clear();
    headers_.clear();
    valid_ = false;

    std::istringstream stream(raw_request);

    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);

    request_line >> method_ >> path_ >> version_;

    if (method_.empty() || path_.empty() || version_.empty()) {
        return false;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        size_t colon_pos = line.find(':');

        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = to_lower(trim(line.substr(0, colon_pos)));
        std::string value = trim(line.substr(colon_pos + 1));

        headers_[key] = value;
    }

    valid_ = true;

    size_t body_pos = raw_request.find("\r\n\r\n");

    if (body_pos != std::string::npos) {
        body_ = raw_request.substr(body_pos + 4);
    }

    return true;
}

const std::string& HttpRequest::method() const {
    return method_;
}

const std::string& HttpRequest::path() const {
    return path_;
}

const std::string& HttpRequest::version() const {
    return version_;
}

const std::string& HttpRequest::body() const {
    return body_;
}

std::string HttpRequest::get_header(const std::string& key) const {
    auto it = headers_.find(to_lower(key));

    if (it == headers_.end()) {
        return "";
    }

    return it->second;
}

bool HttpRequest::is_keep_alive() const {
    std::string connection = to_lower(get_header("connection"));

    if (version_ == "HTTP/1.1") {
        return connection != "close";
    }

    return connection == "keep-alive";
}

bool HttpRequest::is_valid() const {
    return valid_;
}

std::string HttpRequest::trim(const std::string& str) {
    size_t start = 0;

    while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }

    size_t end = str.size();

    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }

    return str.substr(start, end - start);
}

std::string HttpRequest::to_lower(const std::string& str) {
    std::string result = str;

    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return result;
}
