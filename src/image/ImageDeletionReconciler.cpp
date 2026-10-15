#include "image/ImageDeletionReconciler.h"

#include <system_error>

ImageDeletionRecoveryStats reconcile_image_deletion_quarantine(
    const std::filesystem::path& image_directory,
    const ImageMetadataProbe& probe,
    const ImageRecoveryCallback& on_restored,
    const ImageRecoveryCallback& on_discarded
) {
    namespace fs = std::filesystem;
    ImageDeletionRecoveryStats stats;
    const fs::path quarantine = image_directory / ".deleting";

    std::error_code ec;
    if (!fs::is_directory(quarantine, ec)) {
        return stats;
    }

    fs::directory_iterator iterator(quarantine, ec);
    fs::directory_iterator end;
    while (!ec && iterator != end) {
        const fs::directory_entry entry = *iterator;
        iterator.increment(ec);

        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            ++stats.unresolved;
            continue;
        }

        const std::string name = entry.path().filename().string();
        if (name.empty() || name == "." || name == "..") {
            ++stats.unresolved;
            continue;
        }

        const ImageMetadataState state = probe(name);
        if (state == ImageMetadataState::Unknown) {
            ++stats.unresolved;
            continue;
        }

        if (state == ImageMetadataState::Present) {
            const fs::path original = image_directory / name;
            if (fs::exists(original, entry_error) || entry_error) {
                ++stats.unresolved;
                continue;
            }
            fs::rename(entry.path(), original, entry_error);
            if (entry_error) {
                ++stats.unresolved;
                continue;
            }
            if (!on_restored(name)) {
                std::error_code restore_error;
                fs::rename(original, entry.path(), restore_error);
                ++stats.unresolved;
                continue;
            }
            ++stats.restored;
            continue;
        }

        if (!on_discarded(name)) {
            ++stats.unresolved;
            continue;
        }
        const fs::path thumbnail = image_directory / "thumbnails" /
            (fs::path(name).stem().string() + ".webp");
        if (fs::exists(thumbnail, entry_error)) {
            fs::remove(thumbnail, entry_error);
            if (entry_error) {
                ++stats.unresolved;
                continue;
            }
        }
        if (!fs::remove(entry.path(), entry_error) || entry_error) {
            ++stats.unresolved;
            continue;
        }
        ++stats.discarded;
    }

    if (ec) {
        ++stats.unresolved;
    }
    return stats;
}
