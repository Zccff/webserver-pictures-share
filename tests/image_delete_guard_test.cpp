#include "image/ImageDeleteGuard.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path root = fs::temp_directory_path() /
        ("mywebserver-delete-guard-" + suffix);
    fs::create_directories(root);

    const std::string name = "sample.jpg";
    {
        std::ofstream(root / name, std::ios::binary) << "image";
        ImageDeleteGuard guard(root, name);
        std::string error;
        expect(guard.stage(error) == ImageDeleteStageStatus::Ready,
            "could not stage an existing image", failures);
        expect(!fs::exists(root / name) && fs::exists(guard.quarantine_path()),
            "staged image was still public", failures);
        expect(guard.rollback(error), "could not rollback staged image", failures);
        expect(fs::exists(root / name) && !fs::exists(guard.quarantine_path()),
            "rollback did not restore the original image", failures);
    }

    {
        ImageDeleteGuard guard(root, name);
        std::string error;
        expect(guard.stage(error) == ImageDeleteStageStatus::Ready,
            "could not restage image", failures);
        expect(guard.discard(error), "could not discard staged image", failures);
        expect(!fs::exists(root / name) && !fs::exists(guard.quarantine_path()),
            "discard left a public or quarantined image", failures);
    }

    {
        ImageDeleteGuard guard(root, "missing.png");
        std::string error;
        expect(guard.stage(error) == ImageDeleteStageStatus::Ready,
            "missing source should not block metadata cleanup", failures);
        expect(!guard.staged(), "missing source was unexpectedly staged", failures);
    }

    {
        const std::string deferred_name = "deferred.jpg";
        std::ofstream(root / deferred_name, std::ios::binary) << "image";
        fs::path quarantine_path;
        {
            ImageDeleteGuard guard(root, deferred_name);
            std::string error;
            expect(guard.stage(error) == ImageDeleteStageStatus::Ready,
                "could not stage deferred recovery fixture", failures);
            quarantine_path = guard.quarantine_path();
            guard.defer_to_recovery();
        }
        expect(!fs::exists(root / deferred_name) && fs::exists(quarantine_path),
            "deferred guard destructor restored a quarantined image", failures);
    }

    {
        const std::string blocked_name = "rollback-blocked.jpg";
        std::ofstream(root / blocked_name, std::ios::binary) << "image";
        ImageDeleteGuard guard(root, blocked_name);
        std::string error;
        expect(guard.stage(error) == ImageDeleteStageStatus::Ready,
            "could not stage rollback-failure fixture", failures);
        fs::create_directory(root / blocked_name);
        expect(!guard.rollback(error),
            "rollback unexpectedly overwrote an incompatible destination",
            failures);
        expect(fs::exists(guard.quarantine_path()),
            "failed rollback discarded the quarantined image", failures);
        fs::remove(root / blocked_name);
    }

    fs::create_directories(root / ".deleting");
    std::ofstream(root / ".deleting" / name, std::ios::binary) << "pending";
    {
        ImageDeleteGuard guard(root, name);
        std::string error;
        expect(guard.stage(error) == ImageDeleteStageStatus::InProgress,
            "existing quarantine was not reported as in progress", failures);
    }

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures != 0) {
        return 2;
    }
    std::cout << "image delete guard test passed\n";
    return 0;
}
