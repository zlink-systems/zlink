/* SPDX-License-Identifier: MPL-2.0 */
#include "zlink/Contracts/Eventing/poller.hpp"

#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Eventing/monitor_access.hpp>
#include <Runtime/Eventing/poller_item_registry.hpp>
#include <Runtime/Eventing/poller_socket_cache.hpp>
#include <Runtime/Eventing/timer_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>

#include <zlink.h>

#include <algorithm>
#include <cerrno>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink
{

namespace
{

poll_source_kind_t to_source_kind (zlink_poller_source_kind_t source_kind_) noexcept
{
    switch (source_kind_) {
        case 2:
            return poll_source_kind_t::fd;
        case 3:
            return poll_source_kind_t::timer;
        case 1:
        default:
            return poll_source_kind_t::socket;
    }
}

[[noreturn]] void throw_poll_wait_failure (zlink_config_result_t error_, int err_)
{
    if (error_ != ZLINK_CONFIG_OK)
        throw recv_error_t (static_cast<recv_result_t> (static_cast<int> (error_)), err_);
    throw recv_error_t (recv_result_t::invalid_handle, err_);
}

} // namespace

struct poller_t::impl
{
    void *poller = nullptr;
    std::vector<std::unique_ptr<poller_item_t>> items;
    std::vector<zlink_poller_event_t> native_events;
    socket_poll_cache_t socket_poll_cache;
    std::unordered_map<const void *, size_t> socket_item_indexes;
    bool socket_native_events_dirty = false;
    size_t native_poller_item_count = 0;

    impl () : poller (zlink_poller_new ()) {}

    ~impl () { destroy_noexcept (); }

    bool can_use_socket_fast_path () const noexcept { return native_poller_item_count == 0; }

    void ensure_open () const
    {
        if (!poller)
            throw config_error_t (config_result_t::invalid_handle, detail::current_errno ());
    }

    void ensure_addable () const { ensure_open (); }

    void delete_items () noexcept
    {
        items.clear ();
        socket_item_indexes.clear ();
    }

    void clear_vectors ()
    {
        delete_items ();
        native_events.clear ();
        socket_poll_cache.clear ();
        socket_native_events_dirty = false;
        native_poller_item_count = 0;
    }

    void destroy_noexcept () noexcept
    {
        clear_vectors ();
        if (!poller)
            return;
        void *handle = poller;
        (void) zlink_poller_destroy (&handle);
        poller = nullptr;
    }

    void close ()
    {
        if (!poller) {
            clear_vectors ();
            return;
        }

        void *handle = poller;
        const close_result_t result = static_cast<close_result_t> (zlink_poller_destroy (&handle));
        detail::throw_if_failed<close_error_t> (result);
        poller = nullptr;
        clear_vectors ();
    }

    int find_socket (const void *socket_handle_) const noexcept
    {
        return find_socket_item (items, socket_item_indexes, socket_handle_);
    }

    int find_fd (int fd_) const noexcept { return find_fd_item (items, fd_); }

    int find_timer (const void *timer_handle_) const noexcept
    {
        return find_timer_item (items, timer_handle_);
    }

    void rebuild_socket_item_indexes ()
    {
        zlink::rebuild_socket_item_indexes (items, socket_item_indexes);
    }

    void erase_item_at (int index_, bool native_poller_item_)
    {
        items.erase (items.begin () + index_);
        rebuild_socket_item_indexes ();
        if (native_poller_item_)
            --native_poller_item_count;
        socket_poll_cache.mark_dirty ();
    }

    void commit_added_item (std::unique_ptr<poller_item_t> item_, config_result_t rc_)
    {
        detail::throw_if_failed<config_error_t> (rc_);
        const bool socket_item = item_->source_kind == poll_source_kind_t::socket;
        const void *socket_handle = item_->socket_handle;
        const size_t index = items.size ();
        items.push_back (std::move (item_));
        if (socket_item)
            socket_item_indexes[socket_handle] = index;
        socket_poll_cache.mark_dirty ();
    }

    void add_socket (void *socket_handle_,
                     poll_event_flag_t events_,
                     std::uintptr_t slot_,
                     bool native_poller_only_)
    {
        ensure_addable ();
        if (native_poller_only_)
            sync_socket_native_events_if_needed ();
        if (find_socket (socket_handle_) >= 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        auto item = std::make_unique<poller_item_t> ();
        item->socket_handle = socket_handle_;
        item->source_kind = poll_source_kind_t::socket;
        item->events = events_;
        item->slot = slot_;
        item->native_poller_only = native_poller_only_;

        poller_item_t *raw_item = item.get ();
        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_add (poller, socket_handle_, raw_item, static_cast<short> (events_)));
        commit_added_item (std::move (item), rc);
        if (native_poller_only_)
            ++native_poller_item_count;
    }

    void add_fd (int fd_, poll_event_flag_t events_, std::uintptr_t slot_)
    {
        ensure_addable ();
        sync_socket_native_events_if_needed ();
        if (find_fd (fd_) >= 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        auto item = std::make_unique<poller_item_t> ();
        item->fd = fd_;
        item->source_kind = poll_source_kind_t::fd;
        item->events = events_;
        item->slot = slot_;

        poller_item_t *raw_item = item.get ();
        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_add_fd (poller, fd_, raw_item, static_cast<short> (events_)));
        commit_added_item (std::move (item), rc);
        ++native_poller_item_count;
    }

    void add_timer (timer_t &timer_, std::uintptr_t slot_)
    {
        ensure_addable ();
        sync_socket_native_events_if_needed ();
        void *timer_handle = detail::native_handle (timer_);
        if (find_timer (timer_handle) >= 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        auto item = std::make_unique<poller_item_t> ();
        item->timer_handle = timer_handle;
        item->timer = &timer_;
        item->source_kind = poll_source_kind_t::timer;
        item->events = poll_event_flag_t::pollin;
        item->slot = slot_;

        poller_item_t *raw_item = item.get ();
        const config_result_t rc =
          static_cast<config_result_t> (zlink_poller_add_timer (poller, timer_handle, raw_item));
        commit_added_item (std::move (item), rc);
        ++native_poller_item_count;
    }

    void modify_socket (void *socket_handle_, poll_event_flag_t events_)
    {
        ensure_open ();
        const int index = find_socket (socket_handle_);
        if (index < 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        if (can_use_socket_fast_path ()) {
            socket_native_events_dirty = true;
            socket_poll_cache.update_if_clean (items, static_cast<size_t> (index), events_);
            return;
        }

        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_modify (poller, socket_handle_, static_cast<short> (events_)));
        detail::throw_if_failed<config_error_t> (rc);
        items[static_cast<size_t> (index)]->events = events_;
    }

    void modify_fd (int fd_, poll_event_flag_t events_)
    {
        ensure_open ();
        const int index = find_fd (fd_);
        if (index < 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_modify_fd (poller, fd_, static_cast<short> (events_)));
        detail::throw_if_failed<config_error_t> (rc);
        items[static_cast<size_t> (index)]->events = events_;
    }

    bool remove_socket (void *socket_handle_)
    {
        ensure_open ();
        const int index = find_socket (socket_handle_);
        if (index < 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        const config_result_t rc =
          static_cast<config_result_t> (zlink_poller_remove (poller, socket_handle_));
        detail::throw_if_failed<config_error_t> (rc);

        const bool native_only = items[static_cast<size_t> (index)]->native_poller_only;
        erase_item_at (index, native_only);
        return true;
    }

    bool remove_timer (timer_t &timer_)
    {
        ensure_open ();
        const int index = find_timer (detail::native_handle (timer_));
        if (index < 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_remove_timer (poller, detail::native_handle (timer_)));
        detail::throw_if_failed<config_error_t> (rc);

        erase_item_at (index, true);
        return true;
    }

    bool remove_fd (int fd_)
    {
        ensure_open ();
        const int index = find_fd (fd_);
        if (index < 0)
            throw config_error_t (config_result_t::invalid_argument, detail::current_errno ());

        const config_result_t rc =
          static_cast<config_result_t> (zlink_poller_remove_fd (poller, fd_));
        detail::throw_if_failed<config_error_t> (rc);

        erase_item_at (index, true);
        return true;
    }

    void fill_event (poll_event_t &event_, const zlink_poller_event_t &native_event_) const noexcept
    {
        const poller_item_t *item = static_cast<const poller_item_t *> (native_event_.user_data);
        event_.source_kind = item ? item->source_kind : to_source_kind (native_event_.source_kind);
        event_.slot = item ? item->slot : 0;
        event_.fd = event_.source_kind == poll_source_kind_t::fd ? native_event_.fd : 0;
        event_.revents = static_cast<poll_event_flag_t> (native_event_.events);
    }

    void fill_event_from_item (poll_event_t &event_,
                               const poller_item_t &item_,
                               short revents_) const noexcept
    {
        event_.source_kind = item_.source_kind;
        event_.slot = item_.slot;
        event_.fd = item_.source_kind == poll_source_kind_t::fd ? item_.fd : 0;
        event_.revents = static_cast<poll_event_flag_t> (revents_);
    }

    size_t wait (poll_event_t *events_, size_t capacity_, long timeout_)
    {
        if (!poller)
            throw recv_error_t (recv_result_t::invalid_handle, detail::current_errno ());
        if (!events_ || capacity_ == 0)
            throw config_error_t (config_result_t::invalid_argument, EINVAL);
        if (items.empty ())
            return 0;
        if (can_use_socket_fast_path ())
            return wait_socket_items (events_, capacity_, timeout_);

        sync_socket_native_events_if_needed ();
        const size_t capacity = std::min (capacity_, items.size ());

        native_events.resize (capacity);
        zlink_config_result_t error = static_cast<zlink_config_result_t> (0);
        const int rc = zlink_poller_wait (poller, &native_events[0], static_cast<int> (capacity),
                                          timeout_, &error);
        if (rc <= 0) {
            if (rc == 0)
                return 0;
            const int err = detail::current_errno ();
            if (err == EINTR || err == EAGAIN) {
                errno = err;
                return 0;
            }
            throw_poll_wait_failure (error, err);
        }

        for (int i = 0; i < rc; ++i)
            fill_event (events_[static_cast<size_t> (i)], native_events[static_cast<size_t> (i)]);
        return static_cast<size_t> (rc);
    }

    void sync_socket_native_events_if_needed ()
    {
        if (!socket_native_events_dirty)
            return;
        for (size_t i = 0; i < items.size (); ++i) {
            const poller_item_t &item = *items[i];
            if (item.source_kind != poll_source_kind_t::socket)
                continue;
            const config_result_t rc = static_cast<config_result_t> (
              zlink_poller_modify (poller, item.socket_handle, static_cast<short> (item.events)));
            detail::throw_if_failed<config_error_t> (rc);
        }
        socket_native_events_dirty = false;
    }

    size_t wait_socket_items (poll_event_t *events_, size_t capacity_, long timeout_)
    {
        const size_t capacity = std::min (capacity_, items.size ());
        socket_poll_cache.rebuild_if_needed (items);

        if (socket_poll_cache.poll_items.empty ())
            return 0;

        socket_poll_cache.reset_revents ();

        zlink_config_result_t error = static_cast<zlink_config_result_t> (0);
        const int rc =
          zlink_poll (&socket_poll_cache.poll_items[0],
                      static_cast<int> (socket_poll_cache.poll_items.size ()), timeout_, &error);
        if (rc <= 0) {
            if (rc == 0)
                return 0;
            const int err = detail::current_errno ();
            if (err == EINTR || err == EAGAIN) {
                errno = err;
                return 0;
            }
            throw_poll_wait_failure (error, err);
        }

        size_t out = 0;
        for (size_t i = 0; i < socket_poll_cache.poll_items.size () && out < capacity; ++i) {
            const short revents = socket_poll_cache.poll_items[i].revents;
            if (revents == 0)
                continue;
            fill_event_from_item (events_[out], *items[socket_poll_cache.item_indexes[i]], revents);
            ++out;
        }
        return out;
    }
};

poller_t::poller_t () : _impl (std::make_unique<impl> ())
{
}

poller_t::~poller_t () = default;

poller_t::poller_t (poller_t &&other_) noexcept = default;

poller_t &poller_t::operator= (poller_t &&other_) noexcept = default;

bool poller_t::valid () const noexcept
{
    return _impl && _impl->poller != nullptr;
}

int poller_t::size () const
{
    if (!valid ())
        throw config_error_t (config_result_t::invalid_handle, EINVAL);
    return static_cast<int> (_impl->items.size ());
}

void poller_t::add_fd (int fd_, poll_event_flag_t events_, std::uintptr_t slot_)
{
    _impl->add_fd (fd_, events_, slot_);
}

void poller_t::add (timer_t &timer_, std::uintptr_t slot_)
{
    _impl->add_timer (timer_, slot_);
}

void poller_t::add (socket_monitor_t &monitor_, poll_event_flag_t events_, std::uintptr_t slot_)
{
    _impl->add_socket (zlink::detail::native_handle (monitor_), events_, slot_, true);
}

void poller_t::add (socket_t &socket_, poll_event_flag_t events_, std::uintptr_t slot_)
{
    const bool native_only =
      (static_cast<short> (events_) & static_cast<short> (poll_event_flag_t::pollcompletion)) != 0;
    _impl->add_socket (zlink::detail::native_handle (socket_), events_, slot_, native_only);
}

void poller_t::modify_fd (int fd_, poll_event_flag_t events_)
{
    _impl->modify_fd (fd_, events_);
}

void poller_t::modify (socket_monitor_t &monitor_, poll_event_flag_t events_)
{
    _impl->modify_socket (zlink::detail::native_handle (monitor_), events_);
}

void poller_t::modify (socket_t &socket_, poll_event_flag_t events_)
{
    _impl->modify_socket (zlink::detail::native_handle (socket_), events_);
}

bool poller_t::remove (timer_t &timer_)
{
    return _impl->remove_timer (timer_);
}

bool poller_t::remove (socket_monitor_t &monitor_)
{
    return _impl->remove_socket (zlink::detail::native_handle (monitor_));
}

bool poller_t::remove (socket_t &socket_)
{
    return _impl->remove_socket (zlink::detail::native_handle (socket_));
}

bool poller_t::remove_fd (int fd_)
{
    return _impl->remove_fd (fd_);
}

size_t poller_t::wait (poll_event_t *events_, size_t capacity_, std::chrono::milliseconds timeout_)
{
    return _impl->wait (events_, capacity_, detail::native_poll_timeout_ms (timeout_));
}

int poll (poll_item_t *items_, size_t count_, std::chrono::milliseconds timeout_)
{
    std::vector<zlink_pollitem_t> native_items (count_);
    for (size_t i = 0; i < count_; ++i) {
        if (items_[i].socket)
            native_items[i].socket = detail::native_handle (*items_[i].socket);
        native_items[i].fd = static_cast<zlink_fd_t> (items_[i].fd);
        native_items[i].events = static_cast<short> (items_[i].events);
    }

    zlink_config_result_t error = static_cast<zlink_config_result_t> (0);
    const int rc = zlink_poll (native_items.empty () ? nullptr : native_items.data (),
                               static_cast<int> (native_items.size ()),
                               detail::native_poll_timeout_ms (timeout_), &error);
    if (rc < 0) {
        if (error != 0)
            throw config_error_t (static_cast<config_result_t> (error), detail::current_errno ());
        throw config_error_t (detail::config_result_from_errno (detail::current_errno ()),
                              detail::current_errno ());
    }

    for (size_t i = 0; i < count_; ++i)
        items_[i].revents = static_cast<poll_event_flag_t> (native_items[i].revents);
    return rc;
}

void poller_t::close ()
{
    if (!_impl)
        return;
    _impl->close ();
}

} // namespace zlink
