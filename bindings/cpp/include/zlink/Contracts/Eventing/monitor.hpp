/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/byte_count.hpp"
#include "../Errors/errors.hpp"
#include "events.hpp"
#include "status.hpp"

#include <memory>
#include <optional>

namespace zlink
{

class socket_t;
class socket_monitor_t;
namespace detail
{
struct monitor_access_t;
} // namespace detail

/**
 * @brief Observes a socket's connection lifecycle events and current status.
 * @note The caller owns this resource and must call close() when done.
 */
class socket_monitor_t
{
  public:
    socket_monitor_t ();

    static socket_monitor_t open (const socket_t &socket_,
                                  monitor_event events_ = monitor_event::all,
                                  byte_count_t monitor_hwm_bytes_ = byte_count_t::bytes (0));

    ~socket_monitor_t ();

    socket_monitor_t (socket_monitor_t &&other) noexcept;

    socket_monitor_t &operator= (socket_monitor_t &&other) noexcept;

    socket_monitor_t (const socket_monitor_t &) = delete;
    socket_monitor_t &operator= (const socket_monitor_t &) = delete;

    bool valid () const noexcept;

    std::optional<monitor_event_t> recv (recv_flags_t flags_ = recv_flags_t::none);

    monitor_status_t status () const;

    void close ();

  private:
    void close_noexcept () noexcept;

    friend class socket_t;
    friend struct detail::monitor_access_t;

    struct impl;
    std::unique_ptr<impl> _impl;
};

} // namespace zlink
