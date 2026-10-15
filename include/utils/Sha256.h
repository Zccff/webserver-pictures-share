#pragma once

#include <string>
#include <string_view>

class Sha256 {
public:
    static std::string digest(std::string_view value);

    static bool digest_file(
        const std::string& path,
        std::string& digest,
        std::string& error
    );
};
