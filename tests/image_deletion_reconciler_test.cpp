#include "image/ImageDeletionReconciler.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message, int& failures) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

}

int main() {
    namespace fs = std::filesystem;
    int failures = 0;
    const fs::path root = fs::temp_directory_path() /
        ("mywebserver-delete-reconcile-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / ".deleting");
    fs::create_directories(root / "thumbnails");

    std::ofstream(root / ".deleting" / "restore.jpg") << "restore";
    std::ofstream(root / ".deleting" / "discard.png") << "discard";
    std::ofstream(root / ".deleting" / "unknown.webp") << "unknown";
    std::ofstream(root / ".deleting" / "restore-fail.jpg") << "restore-fail";
    std::ofstream(root / ".deleting" / "discard-fail.png") << "discard-fail";
    std::ofstream(root / "thumbnails" / "discard.webp") << "thumb";

    std::vector<std::string> restored;
    std::vector<std::string> discarded;
    ImageDeletionRecoveryStats stats = reconcile_image_deletion_quarantine(
        root,
        [](const std::string& name) {
            if (name == "restore.jpg" || name == "restore-fail.jpg")
                return ImageMetadataState::Present;
            if (name == "discard.png" || name == "discard-fail.png")
                return ImageMetadataState::Missing;
            return ImageMetadataState::Unknown;
        },
        [&](const std::string& name) {
            restored.push_back(name);
            return name != "restore-fail.jpg";
        },
        [&](const std::string& name) {
            discarded.push_back(name);
            return name != "discard-fail.png";
        }
    );

    expect(stats.restored == 1 && stats.discarded == 1 && stats.unresolved == 3,
        "recovery statistics were incorrect", failures);
    expect(fs::is_regular_file(root / "restore.jpg") &&
        !fs::exists(root / ".deleting" / "restore.jpg"),
        "uncommitted deletion was not restored", failures);
    expect(!fs::exists(root / ".deleting" / "discard.png") &&
        !fs::exists(root / "thumbnails" / "discard.webp"),
        "committed deletion residue was not discarded", failures);
    expect(fs::is_regular_file(root / ".deleting" / "unknown.webp"),
        "unknown database outcome was modified", failures);
    expect(fs::is_regular_file(root / ".deleting" / "restore-fail.jpg") &&
        !fs::exists(root / "restore-fail.jpg"),
        "failed lifecycle restore did not return the file to quarantine", failures);
    expect(fs::is_regular_file(root / ".deleting" / "discard-fail.png"),
        "failed lifecycle finalization discarded the recovery file", failures);
    expect(restored.size() == 2 && discarded.size() == 2,
        "recovery callbacks were not called exactly once", failures);

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures != 0) return 2;
    std::cout << "image deletion reconciler test passed\n";
    return 0;
}
