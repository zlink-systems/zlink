/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace zlink::framework::detail
{

class crc32c_accumulator_t
{
  public:
    void update (std::span<const std::uint8_t> bytes) noexcept { update_bytes (bytes); }
    void update (std::span<const std::byte> bytes) noexcept { update_bytes (bytes); }
    std::uint32_t value () const noexcept { return ~_state; }

  private:
    template <typename T> void update_bytes (std::span<const T> bytes) noexcept
    {
        static_assert (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>);
        for (const auto byte : bytes) {
            _state ^= static_cast<std::uint8_t> (byte);
            for (int bit = 0; bit < 8; ++bit) {
                const auto mask =
                  static_cast<std::uint32_t> (-static_cast<std::int32_t> (_state & 1u));
                _state = (_state >> 1u) ^ (0x82f63b78u & mask);
            }
        }
    }

    std::uint32_t _state = 0xffffffffu;
};

template <typename T> std::uint32_t crc32c (std::span<const T> bytes) noexcept
{
    crc32c_accumulator_t accumulator;
    accumulator.update (bytes);
    return accumulator.value ();
}

} // namespace zlink::framework::detail
