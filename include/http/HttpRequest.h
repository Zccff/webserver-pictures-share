#pragma once

#include <cstdint>
#include <memory>
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

    bool has_streamed_upload() const;

    const std::string& upload_temp_path() const;

    const std::string& upload_original_name() const;

    uint64_t upload_size() const;

    void attach_streamed_upload(
        std::string temporary_path,
        std::string original_name,
        uint64_t size
    );

private:
    static std::string trim(const std::string& str);

    static std::string to_lower(const std::string& str);

private:
    struct StreamedUpload {
        std::string temporary_path;
        std::string original_name;
        uint64_t size{ 0 };

        ~StreamedUpload();
    };

    std::string method_;
    std::string path_;
    std::string version_;
    std::string body_;

    std::unordered_map<std::string, std::string> headers_;

    bool valid_{ false };
    std::shared_ptr<StreamedUpload> streamed_upload_;
};
