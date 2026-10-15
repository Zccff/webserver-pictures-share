#include "image/ImageProcessor.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

int main() {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_workers = false;
    int completed = 0;

    ImageProcessor processor(
        1,
        1,
        [&](const ImageProcessTask&, std::string&) {
            std::unique_lock<std::mutex> lock(mutex);
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return release_workers; });
            return true;
        }
    );

    auto completion = [&](ImageProcessResult result) {
        if (!result.ok) {
            std::cerr << "successful fake processor reported failure\n";
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
        cv.notify_all();
    };

    if (!processor.try_enqueue({ "one.jpg", "one.webp", completion })) {
        std::cerr << "first processing task was rejected\n";
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                return first_started;
            })) {
            std::cerr << "processor worker did not start\n";
            return 1;
        }
    }

    if (!processor.try_enqueue({ "two", "two.webp", completion })) {
        std::cerr << "one queued task should fit\n";
        return 1;
    }
    if (processor.try_enqueue({ "three", "three.webp", completion })) {
        std::cerr << "bounded processing queue accepted overflow\n";
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_workers = true;
    }
    cv.notify_all();

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                return completed == 2;
            })) {
            std::cerr << "accepted tasks did not complete\n";
            return 1;
        }
    }
    processor.shutdown();

    bool failure_seen = false;
    ImageProcessor failing(
        1,
        1,
        [](const ImageProcessTask&, std::string& error) {
            error = "synthetic failure";
            return false;
        }
    );
    if (!failing.try_enqueue({
            "bad",
            "bad.webp",
            [&](ImageProcessResult result) {
                std::lock_guard<std::mutex> lock(mutex);
                failure_seen = !result.ok &&
                    result.error == "synthetic failure";
                cv.notify_all();
            }
        })) {
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&]() {
                return failure_seen;
            })) {
            std::cerr << "processing failure was not propagated\n";
            return 1;
        }
    }
    failing.shutdown();
    return 0;
}
