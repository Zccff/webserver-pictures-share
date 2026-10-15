#include "threadpool/ThreadPool.h"

#include <chrono>
#include <future>
#include <iostream>

int main() {
    ThreadPool pool(1, 1);
    std::promise<void> first_started;
    std::promise<void> release_first;
    std::shared_future<void> release = release_first.get_future().share();

    if (!pool.try_submit([&]() {
        first_started.set_value();
        release.wait();
    })) {
        std::cerr << "first task was rejected\n";
        return 1;
    }

    if (first_started.get_future().wait_for(std::chrono::seconds(2)) !=
        std::future_status::ready) {
        std::cerr << "worker did not start first task\n";
        release_first.set_value();
        pool.stop();
        return 2;
    }

    if (!pool.try_submit([]() {})) {
        std::cerr << "task within queue capacity was rejected\n";
        release_first.set_value();
        pool.stop();
        return 3;
    }

    if (pool.try_submit([]() {})) {
        std::cerr << "task beyond queue capacity was accepted\n";
        release_first.set_value();
        pool.stop();
        return 4;
    }

    release_first.set_value();
    pool.stop();
    std::cout << "thread pool capacity test passed\n";
    return 0;
}
