#include "image/ImageDeleteGuard.h"

#include <system_error>
#include <utility>

ImageDeleteGuard::ImageDeleteGuard(
    std::filesystem::path image_directory,
    std::string name
)
    : original_path_(image_directory / name),
      quarantine_path_(image_directory / ".deleting" / std::move(name)) {
}

ImageDeleteGuard::~ImageDeleteGuard() {
    if (!staged_ || terminal_) {
        return;
    }
    std::error_code ignored;
    std::filesystem::rename(quarantine_path_, original_path_, ignored);
}

ImageDeleteStageStatus ImageDeleteGuard::stage(std::string& error) {
    namespace fs = std::filesystem;
    error.clear();

    std::error_code ec;
    bool quarantine_exists = fs::exists(quarantine_path_, ec);
    if (ec) {
        error = "could not inspect image deletion quarantine: " + ec.message();
        return ImageDeleteStageStatus::Error;
    }
    if (quarantine_exists) {
        error = "image deletion is already in progress";
        return ImageDeleteStageStatus::InProgress;
    }

    bool original_exists = fs::exists(original_path_, ec);
    if (ec) {
        error = "could not inspect image file: " + ec.message();
        return ImageDeleteStageStatus::Error;
    }
    if (!original_exists) {
        terminal_ = true;
        return ImageDeleteStageStatus::Ready;
    }
    if (!fs::is_regular_file(original_path_, ec) || ec) {
        error = ec
            ? "could not inspect image file: " + ec.message()
            : "image path is not a regular file";
        return ImageDeleteStageStatus::Error;
    }

    fs::create_directories(quarantine_path_.parent_path(), ec);
    if (ec) {
        error = "could not create image deletion quarantine: " + ec.message();
        return ImageDeleteStageStatus::Error;
    }

    fs::rename(original_path_, quarantine_path_, ec);
    if (ec) {
        error = "could not quarantine image file: " + ec.message();
        return ImageDeleteStageStatus::Error;
    }
    staged_ = true;
    return ImageDeleteStageStatus::Ready;
}

bool ImageDeleteGuard::rollback(std::string& error) {
    error.clear();
    if (!staged_ || terminal_) {
        terminal_ = true;
        return true;
    }

    std::error_code ec;
    std::filesystem::rename(quarantine_path_, original_path_, ec);
    if (ec) {
        error = "could not restore quarantined image: " + ec.message();
        return false;
    }
    staged_ = false;
    terminal_ = true;
    return true;
}

bool ImageDeleteGuard::discard(std::string& error) {
    error.clear();
    terminal_ = true;
    if (!staged_) {
        return true;
    }

    std::error_code ec;
    bool removed = std::filesystem::remove(quarantine_path_, ec);
    if (ec || !removed) {
        error = ec
            ? "could not remove quarantined image: " + ec.message()
            : "quarantined image was not found";
        return false;
    }
    staged_ = false;
    return true;
}

void ImageDeleteGuard::defer_to_recovery() {
    terminal_ = true;
}

bool ImageDeleteGuard::staged() const {
    return staged_;
}

const std::filesystem::path& ImageDeleteGuard::quarantine_path() const {
    return quarantine_path_;
}
