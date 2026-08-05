/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Errors/errors.hpp"
#include "../Sockets/socket_contracts.hpp"
#include "poll_event.hpp"
#include "timers.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace zlink
{

/**
 * @brief Multiplexes sockets, file descriptors, and timers.
 * @note The caller owns this resource and must call close() when done.
 */
class poller_t
{
  public:
    poller_t ();
    ~poller_t ();

    poller_t (poller_t &&other_) noexcept;
    poller_t &operator= (poller_t &&other_) noexcept;

    poller_t (const poller_t &) = delete;
    poller_t &operator= (const poller_t &) = delete;

    bool valid () const noexcept;
    int size () const;

    void add (socket_monitor_t &monitor_, poll_event_flag_t events_, std::uintptr_t slot_);
    void add (socket_t &socket_, poll_event_flag_t events_, std::uintptr_t slot_);

    void add_fd (int fd_, poll_event_flag_t events_, std::uintptr_t slot_);
    void add (timer_t &timer_, std::uintptr_t slot_);

    void modify_fd (int fd_, poll_event_flag_t events_);
    void modify (socket_monitor_t &monitor_, poll_event_flag_t events_);
    void modify (socket_t &socket_, poll_event_flag_t events_);

    bool remove (socket_monitor_t &monitor_);
    bool remove (socket_t &socket_);

    bool remove (timer_t &timer_);
    bool remove_fd (int fd_);

    size_t wait (poll_event_t *events_, size_t capacity_, std::chrono::milliseconds timeout_);

    void close ();

  private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

struct poll_item_t
{
    socket_t *socket = nullptr;
    int fd = 0;
    poll_event_flag_t events = poll_event_flag_t::none;
    poll_event_flag_t revents = poll_event_flag_t::none;

    static poll_item_t from_socket (socket_t &socket_, poll_event_flag_t events_)
    {
        poll_item_t item;
        item.socket = &socket_;
        item.events = events_;
        return item;
    }

    static poll_item_t from_fd (int fd_, poll_event_flag_t events_)
    {
        poll_item_t item;
        item.fd = fd_;
        item.events = events_;
        return item;
    }
};

int poll (poll_item_t *items_, size_t count_, std::chrono::milliseconds timeout_);

inline int poll (std::vector<poll_item_t> &items_, std::chrono::milliseconds timeout_)
{
    return poll (items_.empty () ? nullptr : items_.data (), items_.size (), timeout_);
}

} // namespace zlink
