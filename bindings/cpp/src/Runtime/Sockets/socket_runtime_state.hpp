/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_RUNTIME_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_RUNTIME_STATE_HPP_INCLUDED

#include <Runtime/Messaging/completion_owner.hpp>

#include <memory>

namespace zlink::detail
{

struct socket_runtime_state_t : public std::enable_shared_from_this<socket_runtime_state_t>
{
    explicit socket_runtime_state_t (void *socket_) :
        completion (std::make_shared<completion_owner_t> (socket_))
    {
    }

    std::shared_ptr<completion_owner_t> completion;
    std::shared_ptr<const void> reply_owner;
};

} // namespace zlink::detail

#endif
