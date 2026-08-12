#include "auth/SessionManager.h"
#include "db/UserStore.h"
#include "log/AsyncLogger.h"
#include "server/WebServer.h"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    int port = 9006;

    std::string web_root = "../webroot";

    size_t thread_count = 6;

    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    if (argc >= 3) {
        web_root = argv[2];
    }

    if (argc >= 4) {
        thread_count = static_cast<size_t>(std::stoul(argv[3]));
    }

    AsyncLogger::instance().init("server.log");

    if (UserStore::instance().init_from_env()) {
        LOG_INFO("mysql user store initialized");
    }
    else {
        LOG_WARN("mysql user store unavailable: " + UserStore::instance().status_message());
    }

    if (SessionManager::instance().init_from_env()) {
        LOG_INFO("redis session store initialized");
    }
    else {
        LOG_WARN("redis session store unavailable: " + SessionManager::instance().status_message());
    }

    WebServer server(
        port,
        web_root,
        thread_count
    );

    if (!server.init()) {
        std::cerr << "server init failed" << std::endl;
        return 1;
    }

    server.start();

    AsyncLogger::instance().shutdown();

    return 0;
}
