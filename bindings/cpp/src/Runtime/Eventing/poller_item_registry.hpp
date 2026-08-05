/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_EVENTING_POLLER_ITEM_REGISTRY_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_EVENTING_POLLER_ITEM_REGISTRY_HPP_INCLUDED

#include <Runtime/Eventing/poller_socket_cache.hpp>

#include <unordered_map>

namespace zlink
{

inline int find_socket_item (const std::vector<std::unique_ptr<poller_item_t>> &items_,
                             const std::unordered_map<const void *, size_t> &socket_item_indexes_,
                             const void *socket_handle_) noexcept
{
    const auto cached = socket_item_indexes_.find (socket_handle_);
    if (cached != socket_item_indexes_.end ()) {
        const size_t index = cached->second;
        if (index < items_.size () && items_[index]->source_kind == poll_source_kind_t::socket
            && items_[index]->socket_handle == socket_handle_) {
            return static_cast<int> (index);
        }
    }

    for (size_t i = 0; i < items_.size (); ++i) {
        if (items_[i]->source_kind == poll_source_kind_t::socket
            && items_[i]->socket_handle == socket_handle_) {
            return static_cast<int> (i);
        }
    }
    return -1;
}

inline int find_fd_item (const std::vector<std::unique_ptr<poller_item_t>> &items_,
                         int fd_) noexcept
{
    for (size_t i = 0; i < items_.size (); ++i) {
        if (items_[i]->source_kind == poll_source_kind_t::fd && items_[i]->fd == fd_)
            return static_cast<int> (i);
    }
    return -1;
}

inline int find_timer_item (const std::vector<std::unique_ptr<poller_item_t>> &items_,
                            const void *timer_handle_) noexcept
{
    for (size_t i = 0; i < items_.size (); ++i) {
        if (items_[i]->source_kind == poll_source_kind_t::timer
            && items_[i]->timer_handle == timer_handle_)
            return static_cast<int> (i);
    }
    return -1;
}

inline void
rebuild_socket_item_indexes (const std::vector<std::unique_ptr<poller_item_t>> &items_,
                             std::unordered_map<const void *, size_t> &socket_item_indexes_)
{
    socket_item_indexes_.clear ();
    socket_item_indexes_.reserve (items_.size ());
    for (size_t i = 0; i < items_.size (); ++i) {
        const poller_item_t &item = *items_[i];
        if (item.source_kind == poll_source_kind_t::socket)
            socket_item_indexes_[item.socket_handle] = i;
    }
}

} // namespace zlink

#endif
