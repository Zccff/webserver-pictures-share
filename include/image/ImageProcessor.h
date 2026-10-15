#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct ImageProcessResult {
    bool ok{ false };
    std::string error;
};

struct ImageProcessTask {
    std::string source_path;
    std::string thumbnail_path;
    std::function<void(ImageProcessResult)> completion;
};

class ImageProcessor {
public:
    using Processor = std::function<bool(
        const ImageProcessTask& task,
        std::string& error
    )>;

    static ImageProcessor& instance();

    ImageProcessor(
        size_t worker_count,
        size_t queue_capacity,
        Processor processor
    );

    ~ImageProcessor();

    ImageProcessor(const ImageProcessor&) = delete;
    ImageProcessor& operator=(const ImageProcessor&) = delete;

    bool try_enqueue(ImageProcessTask task);

    size_t queue_size() const;

    void shutdown();

    static Processor default_processor();

private:
    void worker_loop();

    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    std::queue<ImageProcessTask> tasks_;
    std::vector<std::thread> workers_;
    size_t queue_capacity_;
    Processor processor_;
    bool stopping_{ false };
};
