/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED
#define ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED

#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink.h>
#include "../Core/routing_id_access.hpp"
#include "../Messaging/received_access.hpp"
#include "../Native/native_receive.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink
{

// Shared implementation helpers for concrete socket entrypoint headers.

namespace detail
{

inline void set_routing_id_or_throw (void *handle_, const routing_id_t &routing_id_)
{
    const int rc = zlink_set_routing_id (handle_, routing_id_.data (), routing_id_.size ());
    if (rc != 0)
        throw config_error_t (static_cast<config_result_t> (rc), zlink_errno ());
}

inline void get_routing_id_or_throw (void *handle_, routing_id_t &routing_id_)
{
    zlink_routing_id_t native;
    std::memset (&native, 0, sizeof (native));
    const int rc = zlink_get_routing_id (handle_, &native);
    if (rc != 0)
        throw config_error_t (static_cast<config_result_t> (rc), zlink_errno ());
    assign_routing_id_native (routing_id_, native);
}

} // namespace detail

} // namespace zlink

#endif
