//=======================================================================
// Self-contained SHA-256 implementation: used only for salted password hashing (see net/auth.hpp)
// pulled in-tree rather than depending on OpenSSL specifically so Src_Simulation keeps its "standard library only" property, 
// unlike Test_Functionnalities/Password_Cryptage's AES approach, which needs -lssl/-lcrypto (or Src_SQL)
//=======================================================================
#ifndef NET_SHA256_HPP
#define NET_SHA256_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sim::net {

class Sha256 {
public:
    static std::array<std::uint8_t, 32> hash(const std::string& input) {
        Sha256 hasher;
        hasher.update(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
        return hasher.finalize();
    }

private:
    std::uint32_t h_[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    std::uint8_t buffer_[64]{};
    std::uint64_t buffer_len_ = 0;
    std::uint64_t total_len_ = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void process_block(const std::uint8_t* block) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    void update(const std::uint8_t* data, std::size_t len) {
        total_len_ += len;
        while (len > 0) {
            std::size_t take = std::min<std::size_t>(len, 64 - buffer_len_);
            std::memcpy(buffer_ + buffer_len_, data, take);
            buffer_len_ += take;
            data += take;
            len -= take;
            if (buffer_len_ == 64) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finalize() {
        std::uint64_t bit_len = total_len_ * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);

        std::uint8_t zero = 0x00;
        while (buffer_len_ != 56) {
            update(&zero, 1);
        }

        std::uint8_t len_bytes[8];
        for (int i = 0; i < 8; ++i) {
            len_bytes[i] = static_cast<std::uint8_t>((bit_len >> (8 * (7 - i))) & 0xFF);
        }
        // bypass update() for the length field so it isn't itself counted into total_len_
        std::memcpy(buffer_ + buffer_len_, len_bytes, 8);
        process_block(buffer_);

        std::array<std::uint8_t, 32> digest{};
        for (int i = 0; i < 8; ++i) {
            digest[i * 4] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xFF);
        }
        return digest;
    }
};

inline std::string to_hex(const std::array<std::uint8_t, 32>& bytes) {
    static const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t b : bytes) {
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

} // namespace sim::net

#endif // NET_SHA256_HPP