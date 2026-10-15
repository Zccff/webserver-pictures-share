#include "image/ImageActivationJournal.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace {

std::filesystem::path marker_path(
    const std::filesystem::path& image_directory,
    const std::string& saved_name
) {
    return image_directory / ".activating" / (saved_name + ".token");
}

}

bool persist_image_activation(
    const std::filesystem::path& image_directory,
    const std::string& saved_name,
    const std::string& upload_token,
    const std::string& username,
    std::string& error
) {
    namespace fs = std::filesystem;
    error.clear();
    std::error_code ec;
    const fs::path final_path = marker_path(image_directory, saved_name);
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) {
        error = "create activation journal failed: " + ec.message();
        return false;
    }

    static std::atomic<unsigned long long> sequence{ 1 };
    const fs::path temporary = final_path.string() + ".tmp." +
        std::to_string(static_cast<long long>(getpid())) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << upload_token << '\n' << username << '\n';
        output.flush();
        if (!output) {
            error = "write activation journal failed";
            fs::remove(temporary, ec);
            return false;
        }
    }

    const int marker_fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (marker_fd < 0 || ::fsync(marker_fd) != 0) {
        const int sync_error = errno;
        if (marker_fd >= 0) ::close(marker_fd);
        error = "sync activation journal failed: " +
            std::error_code(sync_error, std::generic_category()).message();
        fs::remove(temporary, ec);
        return false;
    }
    ::close(marker_fd);

    fs::rename(temporary, final_path, ec);
    if (ec) {
        error = "publish activation journal failed: " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }

    const int directory_fd = ::open(
        final_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0 || ::fsync(directory_fd) != 0) {
        const int sync_error = errno;
        if (directory_fd >= 0) ::close(directory_fd);
        error = "sync activation journal directory failed: " +
            std::error_code(sync_error, std::generic_category()).message();
        return false;
    }
    ::close(directory_fd);
    return true;
}

bool clear_image_activation(
    const std::filesystem::path& image_directory,
    const std::string& saved_name,
    std::string& error
) {
    error.clear();
    std::error_code ec;
    const bool removed = std::filesystem::remove(
        marker_path(image_directory, saved_name), ec);
    if (ec) {
        error = "remove activation journal failed: " + ec.message();
        return false;
    }
    return removed || !std::filesystem::exists(
        marker_path(image_directory, saved_name), ec);
}

std::vector<PendingImageActivation> load_pending_image_activations(
    const std::filesystem::path& image_directory
) {
    namespace fs = std::filesystem;
    std::vector<PendingImageActivation> pending;
    const fs::path directory = image_directory / ".activating";
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return pending;

    for (fs::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error) continue;
        const std::string marker_name = it->path().filename().string();
        constexpr const char* suffix = ".token";
        constexpr size_t suffix_length = 6;
        if (marker_name.size() <= suffix_length ||
            marker_name.compare(marker_name.size() - suffix_length,
                suffix_length, suffix) != 0) continue;
        std::ifstream input(it->path(), std::ios::binary);
        std::string token;
        std::getline(input, token);
        std::string username;
        std::getline(input, username);
        if (!input && token.empty()) continue;
        const std::string saved_name = marker_name.substr(
            0, marker_name.size() - suffix_length);
        if (!saved_name.empty() && !token.empty()) {
            pending.push_back({ saved_name, token, username });
        }
    }
    return pending;
}
