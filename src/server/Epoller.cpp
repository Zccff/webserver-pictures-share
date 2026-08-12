#include "server/Epoller.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

Epoller::Epoller(int max_events)
    : epoll_fd_(-1),
    events_(max_events) {
    epoll_fd_ = epoll_create1(0);

    if (epoll_fd_ < 0) {
        throw std::runtime_error(
            "epoll_create1 failed: " + std::string(std::strerror(errno))
        );
    }
}

Epoller::~Epoller() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

bool Epoller::add_fd(int fd, uint32_t events) {
    epoll_event event{};
    event.data.fd = fd;
    event.events = events;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);

    if (ret < 0) {
        std::cerr << "epoll_ctl ADD failed, fd = " << fd
            << ", errno = " << errno
            << ", reason = " << std::strerror(errno)
            << std::endl;
        return false;
    }

    return true;
}

bool Epoller::add_fd_with_data(int fd, uint32_t events, uint64_t data) {
    epoll_event event{};
    event.data.u64 = data;
    event.events = events;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);

    if (ret < 0) {
        std::cerr << "epoll_ctl ADD failed, fd = " << fd
            << ", errno = " << errno
            << ", reason = " << std::strerror(errno)
            << std::endl;
        return false;
    }

    return true;
}

bool Epoller::mod_fd(int fd, uint32_t events) {
    epoll_event event{};
    event.data.fd = fd;
    event.events = events;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);

    if (ret < 0) {
        std::cerr << "epoll_ctl MOD failed, fd = " << fd
            << ", errno = " << errno
            << ", reason = " << std::strerror(errno)
            << std::endl;
        return false;
    }

    return true;
}

bool Epoller::mod_fd_with_data(int fd, uint32_t events, uint64_t data) {
    epoll_event event{};
    event.data.u64 = data;
    event.events = events;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);

    if (ret < 0) {
        std::cerr << "epoll_ctl MOD failed, fd = " << fd
            << ", errno = " << errno
            << ", reason = " << std::strerror(errno)
            << std::endl;
        return false;
    }

    return true;
}

bool Epoller::del_fd(int fd) {
    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    if (ret < 0) {
        std::cerr << "epoll_ctl DEL failed, fd = " << fd
            << ", errno = " << errno
            << ", reason = " << std::strerror(errno)
            << std::endl;
        return false;
    }

    return true;
}

int Epoller::wait(int timeout_ms) {
    return epoll_wait(
        epoll_fd_,
        events_.data(),
        static_cast<int>(events_.size()),
        timeout_ms
    );
}

int Epoller::get_event_fd(int index) const {
    return events_[index].data.fd;
}

uint32_t Epoller::get_events(int index) const {
    return events_[index].events;
}

uint64_t Epoller::get_event_data(int index) const {
    return events_[index].data.u64;
}
