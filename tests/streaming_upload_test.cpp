#include "http/HttpConnection.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    namespace fs = std::filesystem;
    fs::path temp_dir = fs::path("/tmp") /
        ("myweb-stream-upload-" +
         std::to_string(static_cast<long long>(getpid())));
    fs::create_directories(temp_dir);

    int sockets[2]{ -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return 1;
    }
    int flags = fcntl(sockets[0], F_GETFL, 0);
    fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK);

    std::string payload(512 * 1024, 'x');
    payload[0] = static_cast<char>(0x89);
    payload[1] = 'P';
    payload[2] = 'N';
    payload[3] = 'G';
    const std::string headers =
        "POST /api/upload-image HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: sid=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
        "Content-Type: image/png\r\n"
        "X-File-Name: photo.png\r\n"
        "Content-Length: " + std::to_string(payload.size()) +
        "\r\n\r\n";

    std::thread writer([&]() {
        const std::string request = headers + payload;
        size_t offset = 0;
        while (offset < request.size()) {
            ssize_t n = write(
                sockets[1],
                request.data() + offset,
                request.size() - offset
            );
            if (n > 0) {
                offset += static_cast<size_t>(n);
            }
        }
    });

    std::string temporary_path;
    {
        HttpConnection connection(sockets[0], 17, temp_dir.string());
        HttpRequest request;
        HttpConnection::ReadStatus status =
            HttpConnection::ReadStatus::Incomplete;
        for (int attempt = 0; attempt < 500 &&
             status == HttpConnection::ReadStatus::Incomplete; ++attempt) {
            status = connection.read_request(request);
            if (status == HttpConnection::ReadStatus::Incomplete) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        writer.join();

        if (status != HttpConnection::ReadStatus::RequestReady ||
            !request.has_streamed_upload() ||
            !request.body().empty() ||
            request.upload_original_name() != "photo.png" ||
            request.upload_size() != payload.size()) {
            std::cerr << "direct upload was retained in memory\n";
            return 2;
        }
        temporary_path = request.upload_temp_path();
        if (fs::file_size(temporary_path) != payload.size()) {
            return 3;
        }
        std::ifstream file(temporary_path, std::ios::binary);
        char signature[4]{};
        file.read(signature, sizeof(signature));
        if (static_cast<unsigned char>(signature[0]) != 0x89 ||
            signature[1] != 'P' || signature[2] != 'N' ||
            signature[3] != 'G') {
            return 4;
        }
    }

    close(sockets[0]);
    close(sockets[1]);
    if (fs::exists(temporary_path)) {
        std::cerr << "temporary upload was not cleaned up\n";
        return 5;
    }
    fs::remove_all(temp_dir);
    return 0;
}
