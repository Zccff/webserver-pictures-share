#include "utils/Sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    if (Sha256::digest("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "string digest mismatch\n";
        return 1;
    }

    const std::filesystem::path path = std::filesystem::path("/tmp") /
        ("myweb-sha256-" + std::to_string(static_cast<long long>(getpid())));
    {
        std::ofstream output(path, std::ios::binary);
        const std::string block(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    std::string digest;
    std::string error;
    const bool ok = Sha256::digest_file(path.string(), digest, error);
    std::filesystem::remove(path);
    if (!ok || digest !=
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") {
        std::cerr << "file digest mismatch: " << digest << " " << error << "\n";
        return 2;
    }
    return 0;
}
