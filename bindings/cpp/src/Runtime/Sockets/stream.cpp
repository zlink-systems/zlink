/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Native/message_access.hpp>
#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_callback_state.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

namespace zlink
{

stream_socket_t::stream_socket_t (context_t &ctx_) :
    routed_message_socket_t (ctx_, socket_type::stream)
{
}

send_operation_t stream_socket_t::send (const routing_id_t &target_rid_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_send;
    state_ptr->raw.socket = detail::native_handle (*this);
    state_ptr->raw.target.first_rid = target_rid_;
    return send_operation_t (std::move (state_ptr));
}

int stream_socket_t::recv (received_t &out_, recv_flags_t flags_)
{
    return socket_t::receive (out_, flags_);
}

void stream_socket_t::set_packet_handler (
  std::function<void (const routing_id_t &, message_t &&, message_t &&)> handler_)
{
    detail::socket_callback_state_t &state = callback_state ();
    state.packet_handler = std::move (handler_);
    auto trampoline = [] (void *, const zlink_routing_id_t *source_rid_, zlink_msg_t *header_,
                          zlink_msg_t *body_, void *userdata_) {
        auto *callback_state = static_cast<detail::socket_callback_state_t *> (userdata_);
        if (!callback_state || !callback_state->packet_handler)
            return;
        const routing_id_t source = zlink::detail::routing_id_from_native_pointer (source_rid_);
        message_t header{message_t::no_init_t ()};
        message_t body{message_t::no_init_t ()};
        zlink::detail::adopt_native_message (header, header_);
        zlink::detail::adopt_native_message (body, body_);
        callback_state->packet_handler (source, std::move (header), std::move (body));
    };
    if (zlink_stream_packet_handler (detail::native_handle (*this),
                                     static_cast<zlink_stream_packet_handler_fn> (+trampoline),
                                     &state)
        != 0)
        throw handler_error_t (detail::handler_result_from_errno (detail::current_errno ()),
                               detail::current_errno ());
}

void stream_socket_t::set_routing_id (const routing_id_t &routing_id_)
{
    detail::set_routing_id_or_throw (detail::native_handle (*this), routing_id_);
}

void stream_socket_t::get_routing_id (routing_id_t &routing_id_) const
{
    detail::get_routing_id_or_throw (const_cast<void *> (detail::native_handle (*this)),
                                     routing_id_);
}

} // namespace zlink
