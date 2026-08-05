/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <cstdint>

namespace zlink
{

/// @brief A lossless count of bytes used by HWM and byte-budget options.
class byte_count_t
{
  public:
    static byte_count_t bytes (uint64_t value_) noexcept { return byte_count_t (value_); }

    uint64_t bytes () const noexcept { return _bytes; }

  private:
    explicit byte_count_t (uint64_t value_) noexcept : _bytes (value_) {}

    uint64_t _bytes;
};

} // namespace zlink
