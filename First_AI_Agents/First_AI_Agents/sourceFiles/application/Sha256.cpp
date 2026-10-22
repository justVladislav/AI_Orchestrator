#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ai::application {

namespace {

// Standard FIPS 180-4 SHA-256 constants and round function, implemented
// directly so this project does not need an external crypto dependency for
// one integrity digest.
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotr(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32U - bits));
}

class Sha256Digest final {
public:
    void update(const unsigned char* data, std::size_t length)
    {
        totalBits_ += static_cast<std::uint64_t>(length) * 8U;
        while (length > 0) {
            const std::size_t take = std::min<std::size_t>(64 - bufferSize_, length);
            std::memcpy(buffer_.data() + bufferSize_, data, take);
            bufferSize_ += take;
            data += take;
            length -= take;
            if (bufferSize_ == 64) {
                processBlock(buffer_.data());
                bufferSize_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finalizeHex()
    {
        // Standard SHA-256 padding: one 0x80 byte, zero bytes, then the
        // original bit-length as a big-endian 64-bit integer. The message
        // length used here was already captured in totalBits_ by update();
        // padding bytes below are appended directly to the buffer instead of
        // going back through update(), which would incorrectly count them
        // toward the encoded message length.
        const std::uint64_t messageBits = totalBits_;

        buffer_[bufferSize_++] = 0x80;
        if (bufferSize_ > 56) {
            while (bufferSize_ < 64) {
                buffer_[bufferSize_++] = 0x00;
            }
            processBlock(buffer_.data());
            bufferSize_ = 0;
        }
        while (bufferSize_ < 56) {
            buffer_[bufferSize_++] = 0x00;
        }
        for (int index = 0; index < 8; ++index) {
            buffer_[56 + static_cast<std::size_t>(index)] =
                static_cast<unsigned char>(messageBits >> (56 - 8 * index));
        }
        processBlock(buffer_.data());

        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (const std::uint32_t word : state_) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                const auto byte = static_cast<unsigned char>(word >> shift);
                hex.push_back(kHexDigits[byte >> 4]);
                hex.push_back(kHexDigits[byte & 0x0F]);
            }
        }
        return hex;
    }

private:
    void processBlock(const unsigned char* block)
    {
        std::array<std::uint32_t, 64> w {};
        for (int index = 0; index < 16; ++index) {
            w[static_cast<std::size_t>(index)] =
                (static_cast<std::uint32_t>(block[index * 4]) << 24)
                | (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotr(w[static_cast<std::size_t>(index - 15)], 7)
                ^ rotr(w[static_cast<std::size_t>(index - 15)], 18)
                ^ (w[static_cast<std::size_t>(index - 15)] >> 3);
            const std::uint32_t s1 = rotr(w[static_cast<std::size_t>(index - 2)], 17)
                ^ rotr(w[static_cast<std::size_t>(index - 2)], 19)
                ^ (w[static_cast<std::size_t>(index - 2)] >> 10);
            w[static_cast<std::size_t>(index)] = w[static_cast<std::size_t>(index - 16)] + s0
                + w[static_cast<std::size_t>(index - 7)] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[static_cast<std::size_t>(index)]
                + w[static_cast<std::size_t>(index)];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> buffer_ {};
    std::size_t bufferSize_ {0};
    std::uint64_t totalBits_ {0};
};

} // namespace

std::string sha256Hex(std::string_view data)
{
    Sha256Digest digest;
    digest.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return digest.finalizeHex();
}

} // namespace ai::application
