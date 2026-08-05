/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

namespace zlink
{

/// @brief Flags that modify send behavior; @c dontwait reports back-pressure instead of blocking.
class send_flags_t
{
  public:
    constexpr send_flags_t (int value_ = 0) noexcept : _value (value_) {}

    constexpr operator int () const noexcept { return _value; }

    static const send_flags_t none;
    static const send_flags_t dontwait;

  private:
    int _value;
};

inline constexpr bool operator== (send_flags_t a_, send_flags_t b_) noexcept
{
    return static_cast<int> (a_) == static_cast<int> (b_);
}

inline constexpr bool operator!= (send_flags_t a_, send_flags_t b_) noexcept
{
    return !(a_ == b_);
}

/// @brief Flags that modify receive behavior; @c dontwait returns instead of blocking when no message is available.
class recv_flags_t
{
  public:
    constexpr recv_flags_t (int value_ = 0) noexcept : _value (value_) {}

    constexpr operator int () const noexcept { return _value; }

    static const recv_flags_t none;
    static const recv_flags_t dontwait;

  private:
    int _value;
};

inline constexpr bool operator== (recv_flags_t a_, recv_flags_t b_) noexcept
{
    return static_cast<int> (a_) == static_cast<int> (b_);
}

inline constexpr bool operator!= (recv_flags_t a_, recv_flags_t b_) noexcept
{
    return !(a_ == b_);
}

inline const send_flags_t send_flags_t::none{0};
inline const send_flags_t send_flags_t::dontwait{0x0001u};
inline const recv_flags_t recv_flags_t::none{0};
inline const recv_flags_t recv_flags_t::dontwait{0x0001u};

/// @brief The outcome of a non-blocking send attempt.
enum class send_result_t : int
{
    sent = 0,
    backpressured = 1,
    not_ready = 2
};

/// @brief The outcome of submitting a send or publish.
enum class submit_result_t : int
{
    ok = 0,
    backpressured = 1,
    not_connected = 2,
    not_found = 3,
    terminated = 4,
    invalid_handle = 5,
    invalid_argument = 6,
    not_supported = 7,
    invalid_state = 8,
    thread_violation = 9,
    out_of_memory = 10,
    seq_exhausted = 11,
    internal_error = 12,
    not_admitted = 13
};

/// @brief The outcome of a receive.
enum class recv_result_t : int
{
    ok = 0,
    no_data = 201,
    busy = 202,
    terminated = 203,
    invalid_handle = 204,
    not_supported = 205,
    internal_error = 206
};

} // namespace zlink
