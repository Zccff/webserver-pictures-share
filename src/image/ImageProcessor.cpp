#include "image/ImageProcessor.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

size_t positive_env_size(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
    return parsed == 0 ? fallback : parsed;
}

int positive_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

bool wait_for_child(
    pid_t child,
    int& status,
    std::string& error
) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(positive_env_int(
            "IMAGE_PROCESSOR_TIMEOUT_SECONDS",
            15
        ));
    while (true) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return true;
        }
        if (waited < 0 && errno != EINTR) {
            error = "wait for vipsthumbnail failed";
            return false;
        }
        if (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        kill(child, SIGTERM);
        const auto terminate_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < terminate_deadline) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                error = "vipsthumbnail timed out";
                return false;
            }
            if (waited < 0 && errno != EINTR) {
                error = "wait for timed-out vipsthumbnail failed";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        kill(child, SIGKILL);
        const auto kill_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < kill_deadline) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno == ECHILD)) {
                error = "vipsthumbnail timed out";
                return false;
            }
            if (waited < 0 && errno != EINTR) {
                error = "reap timed-out vipsthumbnail failed";
                return false;
            }
            if (waited < 0 && errno == EINTR) {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // A process stuck in uninterruptible kernel I/O must not pin the
        // bounded image worker forever. A detached reaper owns the wait.
        std::thread([child]() {
            int ignored_status = 0;
            while (waitpid(child, &ignored_status, 0) < 0) {
                if (errno != EINTR) break;
            }
        }).detach();
        error = "vipsthumbnail timed out";
        return false;
    }
}

bool run_vipsthumbnail(
    const ImageProcessTask& task,
    std::string& error
) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(task.thumbnail_path).parent_path(), ec);
    if (ec) {
        error = "create thumbnail directory failed";
        return false;
    }

    static std::atomic<unsigned long long> sequence{ 1 };
    const std::string temporary_path = task.thumbnail_path + ".tmp." +
        std::to_string(static_cast<long long>(getpid())) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".webp";
    const std::string size =
        std::to_string(positive_env_int("THUMBNAIL_MAX_WIDTH", 480)) + "x" +
        std::to_string(positive_env_int("THUMBNAIL_MAX_HEIGHT", 480));
    const std::string output = temporary_path + "[Q=" +
        std::to_string(positive_env_int("THUMBNAIL_WEBP_QUALITY", 82)) +
        ",strip]";

    pid_t child = fork();
    if (child < 0) {
        error = "fork vipsthumbnail failed";
        return false;
    }
    if (child == 0) {
        execlp(
            "vipsthumbnail",
            "vipsthumbnail",
            task.source_path.c_str(),
            "--size",
            size.c_str(),
            "--rotate",
            "--output",
            output.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }

    int status = 0;
    if (!wait_for_child(child, status, error)) {
        fs::remove(temporary_path, ec);
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !fs::is_regular_file(temporary_path, ec)) {
        fs::remove(temporary_path, ec);
        error = WIFEXITED(status) && WEXITSTATUS(status) == 127
            ? "vipsthumbnail executable is unavailable"
            : "libvips thumbnail generation failed";
        return false;
    }

    fs::rename(temporary_path, task.thumbnail_path, ec);
    if (ec) {
        fs::remove(temporary_path, ec);
        error = "publish thumbnail failed";
        return false;
    }
    if (!fs::is_regular_file(task.source_path, ec)) {
        fs::remove(task.thumbnail_path, ec);
        error = "source image was deleted during thumbnail generation";
        return false;
    }
    return true;
}

}

ImageProcessor& ImageProcessor::instance() {
    static ImageProcessor processor(
        positive_env_size("IMAGE_PROCESSOR_THREADS", 2),
        positive_env_size("IMAGE_PROCESSOR_QUEUE_CAPACITY", 256),
        default_processor()
    );
    return processor;
}

ImageProcessor::ImageProcessor(
    size_t worker_count,
    size_t queue_capacity,
    Processor processor
)
    : queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity),
      processor_(std::move(processor)) {
    if (worker_count == 0) {
        worker_count = 1;
    }
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&ImageProcessor::worker_loop, this);
    }
}

ImageProcessor::~ImageProcessor() {
    shutdown();
}

bool ImageProcessor::try_enqueue(ImageProcessTask task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= queue_capacity_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    not_empty_cv_.notify_one();
    return true;
}

size_t ImageProcessor::queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ImageProcessor::shutdown() {
    std::queue<ImageProcessTask> cancelled_tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        tasks_.swap(cancelled_tasks);
    }
    while (!cancelled_tasks.empty()) {
        ImageProcessTask task = std::move(cancelled_tasks.front());
        cancelled_tasks.pop();
        if (task.completion) {
            try {
                task.completion({ false, "image processor is shutting down" });
            }
            catch (...) {
            }
        }
    }
    not_empty_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

ImageProcessor::Processor ImageProcessor::default_processor() {
    return run_vipsthumbnail;
}

void ImageProcessor::worker_loop() {
    while (true) {
        ImageProcessTask task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_cv_.wait(lock, [this]() {
                return stopping_ || !tasks_.empty();
            });
            if (tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        ImageProcessResult result;
        try {
            result.ok = processor_(task, result.error);
        }
        catch (const std::exception& exception) {
            result.ok = false;
            result.error = exception.what();
        }
        catch (...) {
            result.ok = false;
            result.error = "unknown image processor failure";
        }

        if (task.completion) {
            try {
                task.completion(std::move(result));
            }
            catch (...) {
            }
        }
    }
}
