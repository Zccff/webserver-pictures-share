#include "http/HttpResponse.h"

#include <chrono>
#include <filesystem>
#include <fstream>

bool HttpResponse::image_file_matches_name(
    const std::string& filename,
    const std::string& path
) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    char prefix[12]{};
    input.read(prefix, sizeof(prefix));
    return image_content_matches_name(
        filename,
        std::string(prefix, static_cast<size_t>(input.gcount()))
    );
}

bool HttpResponse::publish_streamed_upload(
    const std::string& saved_name,
    const std::string& temporary_path,
    std::string& error
) const {
    namespace fs = std::filesystem;
    if (saved_name.empty() || safe_filename(saved_name) != saved_name ||
        !is_allowed_image_name(saved_name) || temporary_path.empty()) {
        error = "invalid streamed upload";
        return false;
    }

    std::error_code ec;
    fs::path directory(images_dir());
    fs::create_directories(directory, ec);
    if (ec) {
        error = "create images directory failed";
        return false;
    }

    const fs::path source(temporary_path);
    const fs::path destination = directory / saved_name;
    if (!fs::is_regular_file(source, ec) || ec) {
        error = "uploaded temporary file is missing";
        return false;
    }
    if (fs::exists(destination, ec)) {
        error = "image file already exists";
        return false;
    }
    fs::permissions(
        source,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace,
        ec);
    if (ec) {
        error = "make uploaded image readable failed";
        return false;
    }

    fs::rename(source, destination, ec);
    if (!ec) {
        return true;
    }

    // The configured temp directory normally lives under images/.upload-tmp,
    // so rename is atomic. This fallback handles a cross-filesystem temp path
    // without reading the image into process memory.
    ec.clear();
    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const fs::path staging = directory /
        ("." + saved_name + "." +
         std::to_string(static_cast<long long>(suffix)) + ".publishing");
    fs::copy_file(source, staging, fs::copy_options::none, ec);
    if (ec) {
        error = "copy upload into image storage failed";
        return false;
    }
    fs::permissions(
        staging,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace,
        ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove(staging, cleanup_error);
        error = "make staged image readable failed";
        return false;
    }
    if (fs::exists(destination, ec)) {
        fs::remove(staging, ec);
        error = "image file already exists";
        return false;
    }
    fs::rename(staging, destination, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove(staging, cleanup_error);
        error = "publish upload file failed";
        return false;
    }
    fs::remove(source, ec);
    return true;
}
