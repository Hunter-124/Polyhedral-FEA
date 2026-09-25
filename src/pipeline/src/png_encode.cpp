// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/surface_render.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace polymesh::pipeline {
namespace {

// Self-contained so the CLI gains no image-library dependency: Sub-filtered
// rows (a vertical background gradient is horizontally constant, so Sub turns
// most of the image into zeros) fed to a fixed-Huffman DEFLATE block with a
// bounded greedy LZ77 match search. Deterministic by construction — the same
// pixels always produce the same bytes.

constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
  public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    /// Plain DEFLATE integer field: least-significant bit first.
    void bits(std::uint32_t value, int count) {
        acc_ |= (value & ((1u << count) - 1u)) << bit_count_;
        bit_count_ += count;
        while (bit_count_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFu));
            acc_ >>= 8;
            bit_count_ -= 8;
        }
    }
    /// Huffman code: most-significant bit of the code first.
    void code(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bits((value >> i) & 1u, 1);
        }
    }
    void flush() {
        if (bit_count_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFu));
            acc_ = 0;
            bit_count_ = 0;
        }
    }

  private:
    std::vector<std::uint8_t>& out_;
    std::uint32_t acc_ = 0;
    int bit_count_ = 0;
};

/// Fixed literal/length tree of RFC 1951 §3.2.6.
void put_symbol(BitWriter& w, unsigned symbol) {
    if (symbol < 144) {
        w.code(0x30u + symbol, 8);
    } else if (symbol < 256) {
        w.code(0x190u + symbol - 144u, 9);
    } else if (symbol < 280) {
        w.code(symbol - 256u, 7);
    } else {
        w.code(0xC0u + symbol - 280u, 8);
    }
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/// Single fixed-Huffman DEFLATE block over `data`, wrapped in a zlib stream.
std::vector<std::uint8_t> zlib_deflate(const std::vector<std::uint8_t>& data) {
    constexpr int kHashBits = 15;
    constexpr std::int32_t kHashMask = (1 << kHashBits) - 1;
    constexpr std::int32_t kWindow = 32768;
    constexpr int kMaxChain = 24; // bounded: this is a screenshot, not an archive
    constexpr std::size_t kMinMatch = 3;
    constexpr std::size_t kMaxMatch = 258;

    std::vector<std::uint8_t> out;
    out.reserve(data.size() / 4 + 64);
    out.push_back(0x78); // CMF: deflate, 32 KiB window
    out.push_back(0x01); // FLG: fastest, no dict, (0x7801 % 31) == 0

    std::vector<std::int32_t> head(static_cast<std::size_t>(kHashMask) + 1, -1);
    std::vector<std::int32_t> prev(data.size(), -1);
    const auto hash_at = [&](std::size_t i) {
        return static_cast<std::int32_t>((static_cast<std::uint32_t>(data[i]) << 10 ^
                                          static_cast<std::uint32_t>(data[i + 1]) << 5 ^
                                          static_cast<std::uint32_t>(data[i + 2])) &
                                         static_cast<std::uint32_t>(kHashMask));
    };

    BitWriter w(out);
    w.bits(1, 1); // BFINAL
    w.bits(1, 2); // BTYPE = fixed Huffman

    std::size_t i = 0;
    while (i < data.size()) {
        std::size_t best_len = 0;
        std::size_t best_dist = 0;
        if (i + kMinMatch <= data.size()) {
            const std::int32_t h = hash_at(i);
            std::int32_t candidate = head[static_cast<std::size_t>(h)];
            const std::size_t limit = std::min(kMaxMatch, data.size() - i);
            for (int chain = 0; chain < kMaxChain && candidate >= 0; ++chain) {
                const std::size_t c = static_cast<std::size_t>(candidate);
                if (i - c > static_cast<std::size_t>(kWindow)) {
                    break;
                }
                std::size_t len = 0;
                while (len < limit && data[c + len] == data[i + len]) {
                    ++len;
                }
                if (len > best_len) {
                    best_len = len;
                    best_dist = i - c;
                    if (best_len == limit) {
                        break;
                    }
                }
                candidate = prev[c];
            }
            // Insert this position (and, for a match, the positions it covers)
            // so later matches can still find them.
            const std::size_t insert_end =
                std::min(i + std::max<std::size_t>(best_len, 1), data.size() - kMinMatch + 1);
            for (std::size_t k = i; k < insert_end; ++k) {
                const std::int32_t kh = hash_at(k);
                prev[k] = head[static_cast<std::size_t>(kh)];
                head[static_cast<std::size_t>(kh)] = static_cast<std::int32_t>(k);
            }
        }
        if (best_len < kMinMatch) {
            put_symbol(w, data[i]);
            ++i;
            continue;
        }
        unsigned li = 28;
        while (li > 0 && kLenBase[li] > best_len) {
            --li;
        }
        put_symbol(w, 257u + li);
        w.bits(static_cast<std::uint32_t>(best_len - kLenBase[li]), kLenExtra[li]);
        unsigned di = 29;
        while (di > 0 && kDistBase[di] > best_dist) {
            --di;
        }
        w.code(di, 5);
        w.bits(static_cast<std::uint32_t>(best_dist - kDistBase[di]), kDistExtra[di]);
        i += best_len;
    }
    put_symbol(w, 256); // end of block
    w.flush();

    const std::uint32_t sum = adler32(data);
    out.push_back(static_cast<std::uint8_t>(sum >> 24));
    out.push_back(static_cast<std::uint8_t>((sum >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((sum >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(sum & 0xFFu));
    return out;
}

void push_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void push_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
                const std::vector<std::uint8_t>& payload) {
    push_be32(out, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> crc_input;
    crc_input.reserve(payload.size() + 4);
    for (int i = 0; i < 4; ++i) {
        crc_input.push_back(static_cast<std::uint8_t>(type[i]));
    }
    crc_input.insert(crc_input.end(), payload.begin(), payload.end());
    out.insert(out.end(), crc_input.begin(), crc_input.begin() + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    push_be32(out, crc32(crc_input.data(), crc_input.size()));
}

} // namespace

bool write_png(const std::string& path, const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.rgb.size() != static_cast<std::size_t>(image.width) *
                                static_cast<std::size_t>(image.height) * 3) {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(image.width) * 3;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(image.height));
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(1); // Sub filter: the gradient background collapses to zeros
        const std::uint8_t* row = image.rgb.data() + static_cast<std::size_t>(y) * stride;
        for (std::size_t x = 0; x < stride; ++x) {
            const std::uint8_t left = x >= 3 ? row[x - 3] : 0;
            raw.push_back(static_cast<std::uint8_t>((row[x] - left) & 0xFFu));
        }
    }

    std::vector<std::uint8_t> png{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    ihdr.reserve(13);
    push_be32(ihdr, static_cast<std::uint32_t>(image.width));
    push_be32(ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // colour type: truecolour RGB
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter method 0
    ihdr.push_back(0); // no interlace
    push_chunk(png, "IHDR", ihdr);
    push_chunk(png, "IDAT", zlib_deflate(raw));
    push_chunk(png, "IEND", {});

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(png.data(), 1, png.size(), file);
    const bool closed = std::fclose(file) == 0;
    return closed && written == png.size();
}

} // namespace polymesh::pipeline
