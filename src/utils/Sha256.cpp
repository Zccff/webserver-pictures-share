#include "utils/Sha256.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

constexpr std::array<uint32_t, 64> K{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

uint32_t rotate_right(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

class State {
public:
    void update(const unsigned char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            block_[block_size_++] = data[i];
            if (block_size_ == block_.size()) {
                transform();
                bit_length_ += 512;
                block_size_ = 0;
            }
        }
    }

    std::string finish() {
        uint64_t total_bits = bit_length_ +
            static_cast<uint64_t>(block_size_) * 8U;
        block_[block_size_++] = 0x80U;

        if (block_size_ > 56) {
            while (block_size_ < 64) {
                block_[block_size_++] = 0;
            }
            transform();
            block_size_ = 0;
        }
        while (block_size_ < 56) {
            block_[block_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_size_++] = static_cast<unsigned char>(
                (total_bits >> shift) & 0xffU
            );
        }
        transform();

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (uint32_t value : hash_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    void transform() {
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t j = i * 4;
            words[i] =
                (static_cast<uint32_t>(block_[j]) << 24U) |
                (static_cast<uint32_t>(block_[j + 1]) << 16U) |
                (static_cast<uint32_t>(block_[j + 2]) << 8U) |
                static_cast<uint32_t>(block_[j + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate_right(words[i - 15], 7) ^
                rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3U);
            const uint32_t s1 = rotate_right(words[i - 2], 17) ^
                rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = hash_[0];
        uint32_t b = hash_[1];
        uint32_t c = hash_[2];
        uint32_t d = hash_[3];
        uint32_t e = hash_[4];
        uint32_t f = hash_[5];
        uint32_t g = hash_[6];
        uint32_t h = hash_[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate_right(e, 6) ^
                rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t choose = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + choose + K[i] + words[i];
            const uint32_t s0 = rotate_right(a, 2) ^
                rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        hash_[0] += a;
        hash_[1] += b;
        hash_[2] += c;
        hash_[3] += d;
        hash_[4] += e;
        hash_[5] += f;
        hash_[6] += g;
        hash_[7] += h;
    }

    std::array<unsigned char, 64> block_{};
    size_t block_size_{ 0 };
    uint64_t bit_length_{ 0 };
    std::array<uint32_t, 8> hash_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
};

}  // namespace

std::string Sha256::digest(std::string_view value) {
    State state;
    state.update(
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size()
    );
    return state.finish();
}

bool Sha256::digest_file(
    const std::string& path,
    std::string& digest_value,
    std::string& error
) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "could not open uploaded file";
        return false;
    }

    State state;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            state.update(
                reinterpret_cast<const unsigned char*>(buffer.data()),
                static_cast<size_t>(count)
            );
        }
    }
    if (!input.eof()) {
        error = "could not read uploaded file";
        return false;
    }
    digest_value = state.finish();
    error.clear();
    return true;
}
