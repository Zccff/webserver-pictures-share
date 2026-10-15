#include "timer/HeapTimer.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    HeapTimer timer;
    int old_callback_count = 0;
    int adjusted_callback_count = 0;
    int second_callback_count = 0;

    timer.add_or_adjust(1, 1, [&]() { ++old_callback_count; });

    for (int i = 0; i < 1000; ++i) {
        timer.add_or_adjust(1, 100, [&]() { ++adjusted_callback_count; });
    }

    if (timer.size() != 1) {
        std::cerr << "adjustment grew the heap beyond active fd count\n";
        return 1;
    }

    timer.add_or_adjust(2, 1, [&]() { ++second_callback_count; });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    timer.tick();

    if (old_callback_count != 0 || adjusted_callback_count != 0 ||
        second_callback_count != 1) {
        std::cerr << "timer adjustment kept a stale node\n";
        return 2;
    }

    timer.remove(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    timer.tick();

    if (adjusted_callback_count != 0 || timer.get_next_timeout_ms() != -1) {
        std::cerr << "timer removal failed\n";
        return 3;
    }

    std::cout << "heap timer test passed\n";
    return 0;
}
