/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"
#include "message.hpp"

#include <atomic>
#include <optional>

namespace zlink
{

class stream_socket_t;

/// Reusable output storage for one STREAM packet.
class stream_packet_t final
{
  public:
    stream_packet_t () = default;
    ~stream_packet_t ();

    stream_packet_t (stream_packet_t &&other_) noexcept;
    stream_packet_t &operator= (stream_packet_t &&other_) noexcept;
    stream_packet_t (const stream_packet_t &) = delete;
    stream_packet_t &operator= (const stream_packet_t &) = delete;

    bool empty () const noexcept;
    const std::optional<routing_id_t> &routing_id () const noexcept;
    message_t &header ();
    message_t &body ();
    void close () noexcept;

  private:
    std::optional<routing_id_t> _routing_id;
    std::optional<message_t> _header;
    std::optional<message_t> _body;
    std::atomic<bool> _receiving{false};

    friend class stream_socket_t;
};

} // namespace zlink
