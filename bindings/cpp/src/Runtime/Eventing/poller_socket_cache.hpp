/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_EVENTING_POLLER_SOCKET_CACHE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_EVENTING_POLLER_SOCKET_CACHE_HPP_INCLUDED

#include <zlink/Contracts/Eventing/poll_event.hpp>

#include <zlink.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace zlink
{

class timer_t;
namespace detail { class completion_owner_t; }

struct poller_item_t
{
    void *socket_handle = nullptr;
    int fd = 0;
    void *timer_handle = nullptr;
    timer_t *timer = nullptr;
    poll_source_kind_t source_kind = poll_source_kind_t::socket;
    poll_event_flag_t events = poll_event_flag_t::none;
    std::uintptr_t slot = 0;
    bool native_poller_only = false;
    std::shared_ptr<detail::completion_owner_t> completion_owner;
    bool owns_completion = false;
};

struct socket_poll_cache_t
{
    std::vector<zlink_pollitem_t> poll_items;
    std::vector<size_t> item_indexes;
    std::vector<size_t> item_positions;
    bool dirty = true;
    static constexpr size_t npos = static_cast<size_t> (-1);

    void clear ()
    {
        poll_items.clear ();
        item_indexes.clear ();
        item_positions.clear ();
        dirty = true;
    }

    void mark_dirty () noexcept { dirty = true; }

    void rebuild_if_needed (const std::vector<std::unique_ptr<poller_item_t>> &items_)
    {
        if (!dirty)
            return;

        poll_items.clear ();
        item_indexes.clear ();
        item_positions.assign (items_.size (), npos);
        poll_items.reserve (items_.size ());
        item_indexes.reserve (items_.size ());

        for (size_t i = 0; i < items_.size (); ++i) {
            const poller_item_t &item = *items_[i];
            if (item.source_kind != poll_source_kind_t::socket || item.native_poller_only)
                continue;
            zlink_pollitem_t poll_item;
            poll_item.socket = item.socket_handle;
            poll_item.fd = 0;
            poll_item.events = static_cast<short> (item.events);
            poll_item.revents = 0;
            item_positions[i] = poll_items.size ();
            poll_items.push_back (poll_item);
            item_indexes.push_back (i);
        }

        dirty = false;
    }

    void reset_revents () noexcept
    {
        for (size_t i = 0; i < poll_items.size (); ++i)
            poll_items[i].revents = 0;
    }

    void update_if_clean (std::vector<std::unique_ptr<poller_item_t>> &items_,
                          size_t index_,
                          poll_event_flag_t events_)
    {
        poller_item_t &item = *items_[index_];
        if (dirty) {
            item.events = events_;
            return;
        }

        if (item_positions.size () != items_.size ()) {
            dirty = true;
            item.events = events_;
            return;
        }

        // POLLER_MODIFY_HOT_PATH: keep a stable cache slot for every socket,
        // including sockets whose current interest mask is empty. Backpressure
        // loops can then toggle interest in O(1) without erase/insert shifts.
        item.events = events_;
        const size_t position = item_positions[index_];
        if (position >= poll_items.size ()) {
            dirty = true;
            return;
        }
        poll_items[position].events = static_cast<short> (events_);
    }
};

} // namespace zlink

#endif
