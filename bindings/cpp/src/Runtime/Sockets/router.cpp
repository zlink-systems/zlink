/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_callback_state.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

namespace zlink
{

router_socket_t::router_socket_t (context_t &ctx_) :
    routed_message_socket_t (ctx_, socket_type::router),
    _default_request_timeout (std::chrono::milliseconds ())
{
}

routed_send_operation_t router_socket_t::send (const routing_id_t &target_rid_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_send;
    state_ptr->raw.socket = detail::native_handle (*this);
    detail::bind_callback_state (state_ptr->raw, callback_state ());
    detail::cache_first_rid_native (state_ptr->raw.target, target_rid_);
    return routed_send_operation_t (std::move (state_ptr));
}

int router_socket_t::recv (received_t &out_, recv_flags_t flags_)
{
    const int rc = socket_t::receive (out_, flags_, false);
    if (rc != 0)
        return rc;
    // The reply/send context (router handle + the routing ids and request
    // sequence already stored on `out_`) is enough to reconstruct the native
    // call lazily at submit time, so no per-receive closures are built here.
    if (out_.routing_id ().has_value ()) {
        void *router_handle_ = detail::native_handle (*this);
        detail::received_access_t::set_socket_rid_send_context (
          out_, router_handle_, callback_state ().shared_from_this ());
    }
    return 0;
}

int router_socket_t::recv (routing_id_t &source_rid_out_, message_t &part_out_, recv_flags_t flags_)
{
    return detail::recv_single_part_routed_message (detail::native_handle (*this), source_rid_out_,
                                                    part_out_, flags_);
}

void router_socket_t::set_routing_id (const routing_id_t &routing_id_)
{
    detail::set_routing_id_or_throw (detail::native_handle (*this), routing_id_);
}

void router_socket_t::get_routing_id (routing_id_t &routing_id_) const
{
    detail::get_routing_id_or_throw (const_cast<void *> (detail::native_handle (*this)),
                                     routing_id_);
}

request_operation_t router_socket_t::request (const routing_id_t &routing_id_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_request;
    state_ptr->raw.socket = detail::native_handle (*this);
    detail::bind_callback_state (state_ptr->raw, callback_state ());
    // HOT PATH: routed request submission needs the native routing id only.
    // Keep the same cached representation as routed send instead of copying
    // the 256-byte public value into each operation state.
    detail::cache_first_rid_native (state_ptr->raw.target, routing_id_);
    return request_operation_t (std::move (state_ptr));
}

reply_operation_t router_socket_t::reply (const routing_id_t &routing_id_,
                                                   uint64_t request_seq_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_reply;
    state_ptr->raw.socket = detail::native_handle (*this);
    detail::bind_callback_state (state_ptr->raw, callback_state ());
    state_ptr->raw.target.first_rid = routing_id_;
    state_ptr->reply.request_seq = request_seq_;
    return reply_operation_t (std::move (state_ptr));
}

} // namespace zlink
