#pragma once

#include <filesystem>
#include <string>

enum class ImageDeleteStageStatus {
    Ready,
    InProgress,
    Error
};

// Moves an image out of the public directory before its database row is
// deleted. The move stays on the same filesystem, so rename is atomic.
class ImageDeleteGuard {
public:
    ImageDeleteGuard(std::filesystem::path image_directory, std::string name);
    ~ImageDeleteGuard();

    ImageDeleteGuard(const ImageDeleteGuard&) = delete;
    ImageDeleteGuard& operator=(const ImageDeleteGuard&) = delete;

    ImageDeleteStageStatus stage(std::string& error);
    bool rollback(std::string& error);
    bool discard(std::string& error);

    // Transfer cleanup ownership to startup/background reconciliation.
    void defer_to_recovery();

    bool staged() const;
    const std::filesystem::path& quarantine_path() const;

private:
    std::filesystem::path original_path_;
    std::filesystem::path quarantine_path_;
    bool staged_{ false };
    bool terminal_{ false };
};
