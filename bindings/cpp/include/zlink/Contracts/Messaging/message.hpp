/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zlink
{

class message_t;
class stream_socket_t;

namespace detail
{
struct message_access_t;
} // namespace detail

namespace advanced
{
class external_message_t;
} // namespace advanced

/**
 * @brief Owns one zlink message payload.
 *
 * @note Sending a message consumes its payload: the native frame is moved into
 *       the transport on a successful send, leaving the instance invalid.
 *       Call close() to release a message that will not be sent.
 *       Copy construction and assignment perform deep copies of the payload.
 */
class message_t
{
  public:
    message_t ();
    explicit message_t (size_t size_);
    ~message_t ();

    message_t (const message_t &other_);
    message_t &operator= (const message_t &other_);
    message_t (message_t &&other_) noexcept;
    message_t &operator= (message_t &&other_) noexcept;

    bool valid () const noexcept;

    /// @brief Allocates a message with writable payload storage of @p size_ bytes.
    static message_t allocate (size_t size_);
    /// @brief Creates a message holding an independent copy of the given bytes.
    static message_t from (const std::vector<uint8_t> &bytes_);
    /// @brief Creates a message holding an independent copy of the given bytes.
    static message_t from (std::span<const std::byte> bytes_);
    /// @brief Creates a message holding an independent copy of the given bytes.
    static message_t from (std::span<const uint8_t> bytes_);
    /// @brief Encodes the string as UTF-8 and copies it into a new message.
    static message_t from (const std::string &text_);

    template <typename T> static message_t from_json (const T &value_);

    template <typename T> static message_t from_messagepack (const T &value_);

    template <typename T> static message_t from_protobuf (const T &value_);

    void init ();
    void init (size_t size_);

    /// @brief Returns a pointer to the payload storage owned by this message.
    /// @note The returned pointer is valid only while this message remains valid (not sent or closed).
    std::byte *data () noexcept;
    const std::byte *data () const noexcept;

    std::span<std::byte> bytes () noexcept;
    std::span<const std::byte> bytes () const noexcept;

    size_t size () const noexcept;
    bool is_empty () const noexcept;
    int ref_count () const noexcept;

    std::vector<uint8_t> to_bytes () const;
    size_t copy_to (std::span<std::byte> destination_) const;
    size_t copy_to (std::span<uint8_t> destination_) const;
    std::string to_string () const;

    template <typename T> T parse_json () const;

    template <typename T> T parse_messagepack () const;

    template <typename T> T parse_protobuf () const;

    void close ();

  private:
    struct no_init_t
    {
    };

    explicit message_t (no_init_t) noexcept;

    friend class advanced::external_message_t;
    friend class stream_socket_t;
    friend struct detail::message_access_t;

    void close_noexcept () noexcept;

    alignas (std::max_align_t) std::array<std::byte, 64> _storage;
    bool _valid;
};

namespace advanced
{
using external_message_free_fn_t = void (*) (void *data_, void *hint_);

class external_message_t
{
  public:
    static message_t from (std::span<const std::byte> bytes_);
    static message_t from (std::span<const uint8_t> bytes_);
    static message_t from (std::span<std::byte> bytes_,
                           external_message_free_fn_t free_fn_,
                           void *hint_ = nullptr);
    static message_t from (std::span<uint8_t> bytes_,
                           external_message_free_fn_t free_fn_,
                           void *hint_ = nullptr);
};
} // namespace advanced

} // namespace zlink
