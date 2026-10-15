#include "image/ImageActivationJournal.h"

#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("mywebserver-activation-journal-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    if (!persist_image_activation(
            root, "sample.webp", "token-123", "alice", error)) {
        std::cerr << error << '\n';
        return 2;
    }
    auto pending = load_pending_image_activations(root);
    if (pending.size() != 1 || pending[0].saved_name != "sample.webp" ||
        pending[0].upload_token != "token-123" || pending[0].username != "alice") {
        std::cerr << "activation journal did not round-trip\n";
        return 3;
    }
    if (!clear_image_activation(root, "sample.webp", error) ||
        !load_pending_image_activations(root).empty()) {
        std::cerr << "activation journal was not cleared\n";
        return 4;
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::cout << "image activation journal test passed\n";
    return 0;
}
