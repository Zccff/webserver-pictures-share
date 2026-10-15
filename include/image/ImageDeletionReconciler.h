#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

enum class ImageMetadataState {
    Present,
    Missing,
    Unknown
};

struct ImageDeletionRecoveryStats {
    size_t restored{ 0 };
    size_t discarded{ 0 };
    size_t unresolved{ 0 };
};

using ImageMetadataProbe =
    std::function<ImageMetadataState(const std::string& saved_name)>;
using ImageRecoveryCallback =
    std::function<bool(const std::string& saved_name)>;

// Reconciles files left in images/.deleting after a process crash. Existing
// metadata means the delete did not commit and the file is restored. Missing
// metadata means the delete committed and the quarantined file is discarded.
ImageDeletionRecoveryStats reconcile_image_deletion_quarantine(
    const std::filesystem::path& image_directory,
    const ImageMetadataProbe& probe,
    const ImageRecoveryCallback& on_restored,
    const ImageRecoveryCallback& on_discarded
);
