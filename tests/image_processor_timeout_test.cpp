#include "image/ImageProcessor.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char directory_template[] = "/tmp/myweb-vips-timeout-XXXXXX";
    char* directory = mkdtemp(directory_template);
    if (!directory) {
        std::cerr << "could not create timeout test directory\n";
        return 1;
    }

    const std::filesystem::path fake_vips =
        std::filesystem::path(directory) / "vipsthumbnail";
    {
        std::ofstream script(fake_vips);
        script << "#!/bin/sh\nexec sleep 30\n";
    }
    chmod(fake_vips.c_str(), 0700);

    const char* current_path = std::getenv("PATH");
    const std::string original_path = current_path ? current_path : "";
    const std::string test_path = std::string(directory) + ":" + original_path;
    setenv("PATH", test_path.c_str(), 1);
    setenv("IMAGE_PROCESSOR_TIMEOUT_SECONDS", "1", 1);

    auto completion = std::make_shared<std::promise<ImageProcessResult>>();
    auto future = completion->get_future();
    const auto started = std::chrono::steady_clock::now();
    ImageProcessor processor(1, 1, ImageProcessor::default_processor());
    if (!processor.try_enqueue({
            "ignored-source",
            (std::filesystem::path(directory) / "out.webp").string(),
            [completion](ImageProcessResult result) {
                completion->set_value(std::move(result));
            }
        })) {
        return 2;
    }

    if (future.wait_for(std::chrono::seconds(4)) != std::future_status::ready) {
        std::cerr << "hung vipsthumbnail was not terminated\n";
        return 3;
    }
    ImageProcessResult result = future.get();
    processor.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    setenv("PATH", original_path.c_str(), 1);
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);

    if (result.ok || result.error.find("timed out") == std::string::npos) {
        std::cerr << "timeout was not reported\n";
        return 4;
    }
    if (elapsed > std::chrono::seconds(4)) {
        std::cerr << "thumbnail timeout exceeded its deadline\n";
        return 5;
    }
    return 0;
}
