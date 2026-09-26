/* SPDX-License-Identifier: MPL-2.0 */
#include "zlink/Contracts/Eventing/poller.hpp"

#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Errors/result_from_errno.hpp>
#include <Runtime/Eventing/monitor_access.hpp>
#include <Runtime/Eventing/poller_item_registry.hpp>
#include <Runtime/Eventing/poller_socket_cache.hpp>
#include <Runtime/Eventing/timer_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>

#include <zlink.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink
{

namespace
{

} // namespace

struct poller_t::impl
{
    struct wait_item_t
    {
        std::shared_ptr<poller_item_t> lifetime;
        poll_source_kind_t source_kind;
        std::uintptr_t slot;
        std::shared_ptr<detail::completion_owner_t> completion_owner;
        bool owns_completion;
    };

    struct wait_snapshot_t
    {
        std::unordered_map<const void *, wait_item_t> entries;
        std::vector<zlink_poller_event_t> native_events;
        std::mutex buffer_mutex;
    };

    std::atomic<void *> poller{nullptr};
    std::vector<std::shared_ptr<poller_item_t>> items;
    std::unordered_map<const void *, size_t> socket_item_indexes;
    mutable std::mutex items_mutex;
    std::shared_ptr<wait_snapshot_t> wait_snapshot;

    impl () : poller (zlink_poller_new ()) {}

    ~impl () { destroy_noexcept (); }

    void delete_items () noexcept
    {
        for (const auto &item : items) {
            if (item->owns_completion)
                item->completion_owner->release_public (this);
        }
        items.clear ();
        socket_item_indexes.clear ();
        wait_snapshot.reset ();
    }

    void clear_vectors ()
    {
        delete_items ();
    }

    void destroy_noexcept () noexcept
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        void *handle = poller.load (std::memory_order_acquire);
        if (handle) {
            if (zlink_poller_destroy (&handle) == ZLINK_CLOSE_OK)
                poller.store (nullptr, std::memory_order_release);
        }
        clear_vectors ();
    }

    void close ()
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        void *handle = poller.load (std::memory_order_acquire);
        if (!handle) {
            clear_vectors ();
            return;
        }

        const close_result_t result = static_cast<close_result_t> (zlink_poller_destroy (&handle));
        detail::throw_if_failed<close_error_t> (result);
        poller.store (nullptr, std::memory_order_release);
        clear_vectors ();
    }

    int size () const
    {
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int result = zlink_poller_size (poller.load (std::memory_order_acquire), &error);
        if (result < 0)
            throw config_error_t (static_cast<config_result_t> (error), detail::current_errno ());
        return result;
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

    void erase_item_at (int index_)
    {
        const poller_item_t &removed = *items[static_cast<size_t> (index_)];
        if (removed.source_kind == poll_source_kind_t::socket)
            socket_item_indexes.erase (removed.socket_handle);
        items.erase (items.begin () + index_);
        for (size_t item_index = static_cast<size_t> (index_); item_index < items.size ();
             ++item_index) {
            const poller_item_t &item = *items[item_index];
            if (item.source_kind != poll_source_kind_t::socket)
                continue;
            const auto found = socket_item_indexes.find (item.socket_handle);
            if (found != socket_item_indexes.end ())
                found->second = item_index;
        }
        wait_snapshot.reset ();
    }

    void commit_added_item (std::shared_ptr<poller_item_t> item_, config_result_t rc_)
    {
        detail::throw_if_failed<config_error_t> (rc_);
        const bool socket_item = item_->source_kind == poll_source_kind_t::socket;
        const void *socket_handle = item_->socket_handle;
        const size_t index = items.size ();
        items.push_back (std::move (item_));
        if (socket_item)
            socket_item_indexes[socket_handle] = index;
        wait_snapshot.reset ();
    }

    void add_socket (void *socket_handle_,
                     poll_event_flag_t events_,
                     std::uintptr_t slot_,
                     bool native_poller_only_,
                     std::shared_ptr<detail::completion_owner_t> completion_owner_ = {})
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        auto item = std::make_shared<poller_item_t> ();
        item->socket_handle = socket_handle_;
        item->source_kind = poll_source_kind_t::socket;
        item->events = events_;
        item->slot = slot_;
        item->completion_owner = std::move (completion_owner_);
        item->owns_completion = native_poller_only_ && item->completion_owner != nullptr;

        items.reserve (items.size () + 1);
        const size_t index = items.size ();
        const int existing = find_socket (socket_handle_);
        const bool existing_completion =
          existing >= 0 && items[static_cast<size_t> (existing)]->owns_completion;
        const auto inserted = socket_item_indexes.emplace (socket_handle_, index);
        poller_item_t *raw_item = item.get ();
        try {
            if (item->owns_completion && !existing_completion)
                item->completion_owner->transfer_to_public (this);
            const config_result_t rc = static_cast<config_result_t> (
              zlink_poller_add (poller.load (std::memory_order_acquire), socket_handle_, raw_item,
                                static_cast<short> (events_)));
            if (rc != config_result_t::ok) {
                if (item->owns_completion && !existing_completion)
                    item->completion_owner->release_public (this);
                detail::throw_if_failed<config_error_t> (rc);
            }
        }
        catch (...) {
            if (inserted.second)
                socket_item_indexes.erase (inserted.first);
            throw;
        }
        items.push_back (std::move (item));
        wait_snapshot.reset ();
    }

    void add_fd (int fd_, poll_event_flag_t events_, std::uintptr_t slot_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        auto item = std::make_shared<poller_item_t> ();
        item->fd = fd_;
        item->source_kind = poll_source_kind_t::fd;
        item->events = events_;
        item->slot = slot_;

        items.reserve (items.size () + 1);
        poller_item_t *raw_item = item.get ();
        const config_result_t rc = static_cast<config_result_t> (zlink_poller_add_fd (
          poller.load (std::memory_order_acquire), fd_, raw_item, static_cast<short> (events_)));
        commit_added_item (std::move (item), rc);
    }

    void add_timer (timer_t &timer_, std::uintptr_t slot_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        void *timer_handle = detail::native_handle (timer_);
        auto item = std::make_shared<poller_item_t> ();
        item->timer_handle = timer_handle;
        item->source_kind = poll_source_kind_t::timer;
        item->events = poll_event_flag_t::pollin;
        item->slot = slot_;

        items.reserve (items.size () + 1);
        poller_item_t *raw_item = item.get ();
        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_add_timer (poller.load (std::memory_order_acquire), timer_handle, raw_item));
        commit_added_item (std::move (item), rc);
    }

    void modify_socket (void *socket_handle_, poll_event_flag_t events_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        const int index = find_socket (socket_handle_);
        if (index < 0) {
            const auto result = static_cast<config_result_t> (
              zlink_poller_modify (poller.load (std::memory_order_acquire), socket_handle_,
                                   static_cast<short> (events_)));
            detail::throw_if_failed<config_error_t> (result);
            throw config_error_t (config_result_t::internal_error);
        }

        poller_item_t &item = *items[static_cast<size_t> (index)];
        const bool had_completion = item.owns_completion;
        const bool wants_completion =
          (static_cast<short> (events_) & static_cast<short> (poll_event_flag_t::pollcompletion))
          != 0;
        std::shared_ptr<detail::completion_owner_t> owner = item.completion_owner;
        if (!had_completion && wants_completion && owner) {
            owner->transfer_to_public (this);
        }

        const config_result_t rc = static_cast<config_result_t> (zlink_poller_modify (
          poller.load (std::memory_order_acquire), socket_handle_, static_cast<short> (events_)));
        if (rc != config_result_t::ok && !had_completion && wants_completion && owner)
            owner->release_public (this);
        detail::throw_if_failed<config_error_t> (rc);
        item.events = events_;
        item.owns_completion = wants_completion && owner != nullptr;
        wait_snapshot.reset ();
        if (had_completion && !wants_completion && owner)
            owner->release_public (this);
    }

    void modify_fd (int fd_, poll_event_flag_t events_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        const config_result_t rc = static_cast<config_result_t> (zlink_poller_modify_fd (
          poller.load (std::memory_order_acquire), fd_, static_cast<short> (events_)));
        detail::throw_if_failed<config_error_t> (rc);
        const int index = find_fd (fd_);
        if (index < 0)
            throw config_error_t (config_result_t::internal_error);
        items[static_cast<size_t> (index)]->events = events_;
        wait_snapshot.reset ();
    }

    bool remove_socket (void *socket_handle_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        const config_result_t rc =
          static_cast<config_result_t> (zlink_poller_remove (
            poller.load (std::memory_order_acquire), socket_handle_));
        detail::throw_if_failed<config_error_t> (rc);
        const int index = find_socket (socket_handle_);
        if (index < 0)
            throw config_error_t (config_result_t::internal_error);

        auto owner = items[static_cast<size_t> (index)]->completion_owner;
        const bool owned_completion =
          items[static_cast<size_t> (index)]->owns_completion;
        erase_item_at (index);
        if (owned_completion)
            owner->release_public (this);
        return true;
    }

    bool remove_timer (timer_t &timer_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        const config_result_t rc = static_cast<config_result_t> (
          zlink_poller_remove_timer (poller.load (std::memory_order_acquire),
                                     detail::native_handle (timer_)));
        detail::throw_if_failed<config_error_t> (rc);
        const int index = find_timer (detail::native_handle (timer_));
        if (index < 0)
            throw config_error_t (config_result_t::internal_error);

        erase_item_at (index);
        return true;
    }

    bool remove_fd (int fd_)
    {
        std::lock_guard<std::mutex> lock (items_mutex);
        const config_result_t rc =
          static_cast<config_result_t> (zlink_poller_remove_fd (
            poller.load (std::memory_order_acquire), fd_));
        detail::throw_if_failed<config_error_t> (rc);
        const int index = find_fd (fd_);
        if (index < 0)
            throw config_error_t (config_result_t::internal_error);

        erase_item_at (index);
        return true;
    }

    void settle_terminated_completion_owners (const wait_snapshot_t &snapshot_)
    {
        for (const auto &entry : snapshot_.entries) {
            const auto &item = entry.second;
            if (!item.owns_completion || !item.completion_owner)
                continue;
            const auto &owner = item.completion_owner;
            try {
                (void) owner->drain ();
            }
            catch (const binding_error_t &failure_) {
                const int failure_errno = failure_.internal_errno ();
                owner->shutdown (failure_errno != 0 ? failure_errno : EIO);
            }
            catch (...) {
                owner->shutdown (EIO);
            }
        }
    }

    size_t wait (poll_event_t *events_, size_t capacity_, long timeout_)
    {
        std::shared_ptr<wait_snapshot_t> snapshot;
        {
            std::lock_guard<std::mutex> lock (items_mutex);
            if (!wait_snapshot) {
                auto next = std::make_shared<wait_snapshot_t> ();
                next->entries.reserve (items.size ());
                next->native_events.resize (std::max (items.size (), size_t{1}));
                for (const auto &item : items)
                    next->entries.emplace (
                      item.get (), wait_item_t{item, item->source_kind, item->slot,
                                               item->completion_owner, item->owns_completion});
                wait_snapshot = std::move (next);
            }
            snapshot = wait_snapshot;
        }
        const size_t bounded_capacity = std::min (capacity_, static_cast<size_t> (INT_MAX));
        const int native_capacity =
          static_cast<int> (std::min (bounded_capacity, snapshot->native_events.size ()));
        std::unique_lock<std::mutex> buffer_lock (snapshot->buffer_mutex, std::try_to_lock);
        std::vector<zlink_poller_event_t> overlapping_events;
        if (!buffer_lock.owns_lock ())
            overlapping_events.resize (snapshot->native_events.size ());
        auto &native_events =
          buffer_lock.owns_lock () ? snapshot->native_events : overlapping_events;
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int rc = zlink_poller_wait (poller.load (std::memory_order_acquire),
                                          events_ ? native_events.data () : nullptr,
                                          native_capacity, timeout_, &error);
        if (rc <= 0) {
            if (rc == 0)
                return 0;
            const int err = detail::current_errno ();
            if (err == ETERM || err == ESHUTDOWN)
                settle_terminated_completion_owners (*snapshot);
            throw config_error_t (static_cast<config_result_t> (error), err);
        }

        size_t out = 0;
        for (int i = 0; i < rc; ++i) {
            const zlink_poller_event_t &native = native_events[static_cast<size_t> (i)];
            const auto found = snapshot->entries.find (native.user_data);
            if (found == snapshot->entries.end ())
                continue;
            const wait_item_t &item = found->second;
            poll_event_t event;
            event.source_kind = item.source_kind;
            event.slot = item.slot;
            event.fd = item.source_kind == poll_source_kind_t::fd ? native.fd : 0;
            event.revents = static_cast<poll_event_flag_t> (native.events);
            const auto completion_owner =
              item.owns_completion
                  && (native.events & static_cast<short> (poll_event_flag_t::pollcompletion))
                ? item.completion_owner
                : std::shared_ptr<detail::completion_owner_t>{};
            if (completion_owner) {
                size_t processed = 0;
                try {
                    processed = completion_owner->drain ();
                }
                catch (const binding_error_t &failure_) {
                    const int failure_errno = failure_.internal_errno ();
                    completion_owner->shutdown (failure_errno != 0 ? failure_errno : EIO);
                    throw;
                }
                catch (...) {
                    completion_owner->shutdown (EIO);
                    throw;
                }
                if (processed == 0)
                    event.revents = static_cast<poll_event_flag_t> (
                      static_cast<short> (event.revents)
                      & ~static_cast<short> (poll_event_flag_t::pollcompletion));
            }
            if (event.revents == poll_event_flag_t::none)
                continue;
            events_[out++] = event;
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
    return _impl
      && _impl->poller.load (std::memory_order_acquire) != nullptr;
}

int poller_t::size () const
{
    if (!_impl)
        throw config_error_t (config_result_t::invalid_handle, EINVAL);
    return _impl->size ();
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
    std::shared_ptr<detail::completion_owner_t> owner;
    const auto runtime = zlink::detail::runtime_state (socket_);
    if (runtime)
        owner = runtime->completion;
    _impl->add_socket (zlink::detail::native_handle (socket_), events_, slot_, native_only,
                       std::move (owner));
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
        throw config_error_t (
          detail::result_from_errno (config_result_t{}, detail::current_errno ()),
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
