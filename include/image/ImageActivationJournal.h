#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct PendingImageActivation {
    std::string saved_name;
    std::string upload_token;
    std::string username;
};

bool persist_image_activation(
    const std::filesystem::path& image_directory,
    const std::string& saved_name,
    const std::string& upload_token,
    const std::string& username,
    std::string& error
);

bool clear_image_activation(
    const std::filesystem::path& image_directory,
    const std::string& saved_name,
    std::string& error
);

std::vector<PendingImageActivation> load_pending_image_activations(
    const std::filesystem::path& image_directory
);
