#pragma once

#include <cstdint>
#include <sys/epoll.h>
#include <vector>

class Epoller {
public:
    explicit Epoller(int max_events = 1024);

    ~Epoller();

    Epoller(const Epoller&) = delete;
    Epoller& operator=(const Epoller&) = delete;

    bool add_fd(int fd, uint32_t events);

    bool add_fd_with_data(int fd, uint32_t events, uint64_t data);

    bool mod_fd(int fd, uint32_t events);

    bool mod_fd_with_data(int fd, uint32_t events, uint64_t data);

    bool del_fd(int fd);

    int wait(int timeout_ms = -1);

    int get_event_fd(int index) const;

    uint32_t get_events(int index) const;

    uint64_t get_event_data(int index) const;

private:
    int epoll_fd_;
    std::vector<epoll_event> events_;
};
