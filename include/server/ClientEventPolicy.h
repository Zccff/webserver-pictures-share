#pragma once

#include <cstdint>
#include <sys/epoll.h>

namespace ClientEventPolicy {

inline constexpr uint32_t READ_EVENTS =
    EPOLLIN | EPOLLRDHUP | EPOLLONESHOT | EPOLLET;
inline constexpr uint32_t WRITE_EVENTS =
    EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT | EPOLLET;

}
