/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* LZ4 "pickle" payload framing — the STREAM compression wire the other
 * languages already speak (.NET `LZ4Pickler`, Node `lz4-pickle.ts`). The frame
 * is [header byte][size-diff (little endian, 0/1/2/4 bytes)][LZ4 block]: the
 * top two header bits encode the width of the difference between the
 * decompressed and the compressed length, and a zero difference means the body
 * is stored uncompressed. A raw [u32][block] framing does not interoperate. */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ZLINK_LZ4_PICKLE_WITH_LZ4
#include <lz4.h>
#endif

namespace zlink::detail::lz4_pickle
{

inline std::string pickle (std::span<const std::byte> input)
{
#ifndef ZLINK_LZ4_PICKLE_WITH_LZ4
    (void) input;
    throw std::runtime_error ("LZ4 compression is not linked into this build");
#else
    if (input.empty ()) {
        return {};
    }
    /* The LZ4 API takes int sizes: anything past that boundary cannot be
     * represented and must be rejected before any allocation. */
    if (input.size () > static_cast<std::size_t> (LZ4_MAX_INPUT_SIZE)) {
        throw std::runtime_error ("LZ4 input exceeds fixed size limit");
    }
    const int bound = LZ4_compressBound (static_cast<int> (input.size ()));
    std::vector<char> compressed (static_cast<std::size_t> (bound));
    const auto *input_data = reinterpret_cast<const char *> (input.data ());
    const int written = LZ4_compress_default (input_data, compressed.data (),
                                              static_cast<int> (input.size ()), bound);
    if (written <= 0) {
        throw std::runtime_error ("LZ4 compression failed");
    }

    const auto compressed_size = static_cast<std::size_t> (written);
    /* A block that did not shrink is stored verbatim (diff == 0), which is what
     * the peer codecs emit for incompressible payloads. */
    if (compressed_size >= input.size ()) {
        std::string output;
        output.reserve (input.size () + 1);
        output.push_back (0);
        output.append (reinterpret_cast<const char *> (input.data ()), input.size ());
        return output;
    }

    const auto diff = static_cast<std::uint32_t> (input.size () - compressed_size);
    const std::size_t diff_width = diff <= 0xff ? 1 : (diff <= 0xffff ? 2 : 4);
    const std::uint8_t width_bits = diff_width == 4 ? 3 : static_cast<std::uint8_t> (diff_width);

    std::string output;
    output.reserve (1 + diff_width + compressed_size);
    output.push_back (static_cast<char> (width_bits << 6));
    for (std::size_t index = 0; index < diff_width; index++) {
        output.push_back (static_cast<char> ((diff >> (8 * index)) & 0xff));
    }
    output.append (compressed.data (), compressed_size);
    return output;
#endif
}

inline std::string unpickle (std::span<const std::byte> input, std::size_t max_decompressed_size)
{
#ifndef ZLINK_LZ4_PICKLE_WITH_LZ4
    (void) input;
    (void) max_decompressed_size;
    throw std::runtime_error ("LZ4 decompression is not linked into this build");
#else
    if (input.empty ()) {
        return {};
    }
    const auto header = std::to_integer<std::uint8_t> (input[0]);
    if ((header & 0x07) != 0) {
        throw std::runtime_error ("unexpected LZ4 pickle version");
    }
    const auto encoded_width = static_cast<std::size_t> ((header >> 6) & 0x03);
    const std::size_t diff_width = encoded_width == 3 ? 4 : encoded_width;
    const std::size_t data_offset = 1 + diff_width;
    if (input.size () < data_offset) {
        throw std::runtime_error ("LZ4 pickle header is incomplete");
    }

    std::uint64_t diff = 0;
    for (std::size_t index = 0; index < diff_width; index++) {
        diff |= static_cast<std::uint64_t> (std::to_integer<std::uint8_t> (input[1 + index]))
                << (8 * index);
    }
    const auto compressed_size = input.size () - data_offset;
    const auto decompressed_size = static_cast<std::uint64_t> (compressed_size) + diff;
    if (decompressed_size > max_decompressed_size) {
        throw std::runtime_error ("LZ4 payload exceeds configured receive limit");
    }
    /* An adversarial frame can declare any size: reject what the LZ4 API cannot
     * express before allocating the output. */
    const auto int_max = static_cast<std::uint64_t> (std::numeric_limits<int>::max ());
    if (decompressed_size > int_max || static_cast<std::uint64_t> (compressed_size) > int_max) {
        throw std::runtime_error ("LZ4 payload exceeds fixed size limit");
    }

    const auto *data = reinterpret_cast<const char *> (input.data ()) + data_offset;
    if (diff == 0) {
        return std::string (data, compressed_size);
    }
    std::string output (static_cast<std::size_t> (decompressed_size), '\0');
    const int decoded = LZ4_decompress_safe (data, output.data (),
                                             static_cast<int> (compressed_size),
                                             static_cast<int> (output.size ()));
    if (decoded != static_cast<int> (output.size ())) {
        throw std::runtime_error ("LZ4 decompression failed");
    }
    return output;
#endif
}

} // namespace zlink::detail::lz4_pickle
