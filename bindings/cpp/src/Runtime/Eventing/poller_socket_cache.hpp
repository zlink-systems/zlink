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

namespace detail { class completion_owner_t; }

struct poller_item_t
{
    void *socket_handle = nullptr;
    int fd = 0;
    void *timer_handle = nullptr;
    poll_source_kind_t source_kind = poll_source_kind_t::socket;
    poll_event_flag_t events = poll_event_flag_t::none;
    std::uintptr_t slot = 0;
    std::shared_ptr<detail::completion_owner_t> completion_owner;
    bool owns_completion = false;
};

} // namespace zlink

#endif
