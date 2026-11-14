/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace kaorios {

// SHA-256 of the exact bytes consumed by the configuration reader.
inline std::string configSha256(const std::string &bytes) {
    static constexpr uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    std::array<uint32_t, 8> state{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const auto rotate = [](uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    };
    const auto compress = [&](const std::array<unsigned char, 64> &block) {
        uint32_t words[64]{};
        for (unsigned i = 0; i < 16; ++i) {
            for (unsigned j = 0; j < 4; ++j) words[i] = (words[i] << 8) | block[i * 4 + j];
        }
        for (unsigned i = 16; i < 64; ++i) {
            uint32_t s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            uint32_t s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto work = state;
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t sum1 = rotate(work[4], 6) ^ rotate(work[4], 11) ^ rotate(work[4], 25);
            uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
            uint32_t first = work[7] + sum1 + choose + constants[i] + words[i];
            uint32_t sum0 = rotate(work[0], 2) ^ rotate(work[0], 13) ^ rotate(work[0], 22);
            uint32_t majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
            for (unsigned j = 7; j > 0; --j) work[j] = work[j - 1];
            work[4] += first;
            work[0] = first + sum0 + majority;
        }
        for (unsigned i = 0; i < 8; ++i) state[i] += work[i];
    };
    std::array<unsigned char, 64> block{};
    size_t used = 0;
    for (unsigned char byte : bytes) {
        block[used++] = byte;
        if (used == block.size()) { compress(block); used = 0; }
    }
    block[used++] = 0x80;
    if (used > 56) {
        for (size_t i = used; i < block.size(); ++i) block[i] = 0;
        compress(block);
        used = 0;
    }
    for (size_t i = used; i < 56; ++i) block[i] = 0;
    uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    for (unsigned i = 0; i < 8; ++i) block[63 - i] = static_cast<unsigned char>(bits >> (i * 8));
    compress(block);
    std::string result;
    result.reserve(64);
    static constexpr char hex[] = "0123456789abcdef";
    for (uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) result += hex[(word >> shift) & 15];
    }
    return result;
}

} // namespace kaorios
