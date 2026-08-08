// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Minimal, dependency-free 8-bit RGBA PNG writer for GUI screenshots.
//
// The desktop app must be able to dump a frame without dragging libpng/zlib (or
// a vendored stb blob) into the build, so this emits the smallest legal PNG:
//   signature | IHDR | IDAT | IEND
// IDAT carries a zlib stream (RFC 1950) whose DEFLATE payload (RFC 1951) uses
// only *stored* — uncompressed — blocks. That is ~0.1% larger than the raw
// pixels but needs no Huffman/LZ77 machinery, and every PNG decoder accepts it.
// Screenshots are written once on demand, so the size is irrelevant.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace polymesh::gui::png {

/// CRC-32 (IEEE 802.3, reflected poly 0xEDB88320) over one buffer — PNG chunks.
inline std::uint32_t crc32_of(const unsigned char* data, std::size_t n) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/// Adler-32 (RFC 1950 §9), chunked so the modulo runs once per 5552 bytes.
inline std::uint32_t adler32_of(const unsigned char* data, std::size_t n) {
    constexpr std::uint32_t kBase = 65521u;
    constexpr std::size_t kNMax = 5552u; // largest n keeping the sums in 32 bits
    std::uint32_t a = 1u;
    std::uint32_t b = 0u;
    while (n > 0) {
        const std::size_t k = std::min(n, kNMax);
        for (std::size_t i = 0; i < k; ++i) {
            a += data[i];
            b += a;
        }
        a %= kBase;
        b %= kBase;
        data += k;
        n -= k;
    }
    return (b << 16) | a;
}

/// Append a big-endian u32 (PNG's network byte order for lengths/CRCs).
inline void put_u32be(std::vector<unsigned char>& out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xFFu));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<unsigned char>(v & 0xFFu));
}

/// length | type | data | crc(type+data).
inline void put_chunk(std::vector<unsigned char>& out, const char type[4],
                      const unsigned char* data, std::size_t n) {
    put_u32be(out, static_cast<std::uint32_t>(n));
    const std::size_t crc_begin = out.size();
    out.insert(out.end(), type, type + 4);
    if (n > 0) {
        out.insert(out.end(), data, data + n);
    }
    put_u32be(out, crc32_of(out.data() + crc_begin, out.size() - crc_begin));
}

/// Writes `w`x`h` RGBA8 pixels to `path` as a PNG.
/// `rgba_flip_bottom_up` is in OpenGL order (row 0 = bottom of the image, as
/// glReadPixels returns it); rows are flipped here so the file reads top-down.
/// Returns false on bad arguments or any I/O failure.
inline bool write_png_rgba(const char* path, int w, int h,
                           const unsigned char* rgba_flip_bottom_up) {
    if (path == nullptr || rgba_flip_bottom_up == nullptr || w <= 0 || h <= 0) {
        return false;
    }

    // Raw PNG scanlines: one filter byte (0 = None) in front of every row.
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
    const std::size_t raw_size = static_cast<std::size_t>(h) * (row_bytes + 1u);
    std::vector<unsigned char> raw(raw_size);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src =
            rgba_flip_bottom_up + static_cast<std::size_t>(h - 1 - y) * row_bytes;
        unsigned char* dst = raw.data() + static_cast<std::size_t>(y) * (row_bytes + 1u);
        dst[0] = 0;
        std::memcpy(dst + 1, src, row_bytes);
    }

    // zlib stream: CMF 0x78 (deflate, 32K window) + FLG 0x01 (no dict, check ok),
    // then stored DEFLATE blocks of at most 65535 bytes, BFINAL set on the last.
    constexpr std::size_t kBlock = 65535u;
    std::vector<unsigned char> z;
    z.reserve(raw_size + (raw_size / kBlock + 1u) * 5u + 6u);
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t off = 0;
    do {
        const std::size_t len = std::min(raw_size - off, kBlock);
        const bool last = (off + len) >= raw_size;
        z.push_back(last ? 0x01 : 0x00); // BFINAL bit + BTYPE 00, byte-aligned
        const auto len16 = static_cast<std::uint16_t>(len);
        const auto nlen16 = static_cast<std::uint16_t>(~len16);
        z.push_back(static_cast<unsigned char>(len16 & 0xFFu)); // LEN, little-endian
        z.push_back(static_cast<unsigned char>((len16 >> 8) & 0xFFu));
        z.push_back(static_cast<unsigned char>(nlen16 & 0xFFu)); // NLEN = ~LEN
        z.push_back(static_cast<unsigned char>((nlen16 >> 8) & 0xFFu));
        z.insert(z.end(), raw.data() + off, raw.data() + off + len);
        off += len;
    } while (off < raw_size);
    put_u32be(z, adler32_of(raw.data(), raw_size)); // Adler-32 of the raw data

    std::vector<unsigned char> file;
    file.reserve(z.size() + 64u);
    constexpr unsigned char kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    file.insert(file.end(), kSignature, kSignature + 8);

    unsigned char ihdr[13];
    const auto uw = static_cast<std::uint32_t>(w);
    const auto uh = static_cast<std::uint32_t>(h);
    ihdr[0] = static_cast<unsigned char>((uw >> 24) & 0xFFu);
    ihdr[1] = static_cast<unsigned char>((uw >> 16) & 0xFFu);
    ihdr[2] = static_cast<unsigned char>((uw >> 8) & 0xFFu);
    ihdr[3] = static_cast<unsigned char>(uw & 0xFFu);
    ihdr[4] = static_cast<unsigned char>((uh >> 24) & 0xFFu);
    ihdr[5] = static_cast<unsigned char>((uh >> 16) & 0xFFu);
    ihdr[6] = static_cast<unsigned char>((uh >> 8) & 0xFFu);
    ihdr[7] = static_cast<unsigned char>(uh & 0xFFu);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type: truecolor + alpha
    ihdr[10] = 0; // compression: deflate
    ihdr[11] = 0; // filter method: adaptive (per-row byte, all 0 here)
    ihdr[12] = 0; // interlace: none
    put_chunk(file, "IHDR", ihdr, sizeof(ihdr));
    put_chunk(file, "IDAT", z.data(), z.size());
    put_chunk(file, "IEND", nullptr, 0);

    std::FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(file.data(), 1, file.size(), fp);
    const bool flushed = std::fclose(fp) == 0; // always close, even on short write
    return written == file.size() && flushed;
}

} // namespace polymesh::gui::png
