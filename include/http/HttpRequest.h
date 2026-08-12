#pragma once

#include <string>
#include <unordered_map>

class HttpRequest {
public:
    bool parse(const std::string& raw_request);

    const std::string& method() const;

    const std::string& path() const;

    const std::string& version() const;

    const std::string& body() const;

    std::string get_header(const std::string& key) const;

    bool is_keep_alive() const;

    bool is_valid() const;

private:
    static std::string trim(const std::string& str);

    static std::string to_lower(const std::string& str);

private:
    std::string method_;
    std::string path_;
    std::string version_;
    std::string body_;

    std::unordered_map<std::string, std::string> headers_;

    bool valid_{ false };
};
