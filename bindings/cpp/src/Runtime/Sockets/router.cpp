/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_callback_state.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <Runtime/Native/native_message_parts.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

namespace zlink
{

router_socket_t::router_socket_t (context_t &ctx_) :
    routed_message_socket_t (ctx_, socket_type::router),
    _default_request_timeout (std::chrono::milliseconds ())
{
}

send_operation_t router_socket_t::send (const routing_id_t &target_rid_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_send;
    state_ptr->raw.socket = detail::native_handle (*this);
    detail::cache_first_rid_native (state_ptr->raw.target, target_rid_);
    return send_operation_t (std::move (state_ptr));
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
        detail::received_access_t::set_socket_rid_send_context (out_, router_handle_);
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
    state_ptr->raw.target.first_rid = routing_id_;
    state_ptr->reply.request_seq = request_seq_;
    return reply_operation_t (std::move (state_ptr));
}

bool router_socket_t::try_send_completion_control (
  const routing_id_t &peer_rid_, const std::vector<message_t> &parts_)
{
    if (parts_.empty ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const zlink_routing_id_t native_rid =
      *zlink::detail::routing_id_native (peer_rid_);
    const int rc = detail::submit_borrowed_message_array (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          size_t failed_index = 0;
          return detail::submit_native_parts (
            native_parts_, part_count_, failed_index,
            [&] (zlink_msg_t *part_, zlink_part_flag_t flag_, bool) {
                return zlink_router_completion_control_part (
                  detail::native_handle (*this), &native_rid, part_, flag_);
            });
      });

    const submit_result_t result = static_cast<submit_result_t> (rc);
    if (result == submit_result_t::ok)
        return true;
    if (result == submit_result_t::backpressured)
        return false;
    throw submit_error_t (result, zlink_errno ());
}

void router_socket_t::set_completion_control_handler (
  completion_control_handler_t handler_)
{
    if (!handler_)
        throw handler_error_t (handler_result_t::invalid_argument, EINVAL);

    detail::socket_callback_state_t &state = callback_state ();
    auto trampoline = [] (const zlink_routing_id_t *source_rid_,
                          zlink_msg_t *parts_, size_t part_count_,
                          void *userdata_) {
        detail::socket_callback_state_t *callback_state =
          static_cast<detail::socket_callback_state_t *> (userdata_);
        if (!callback_state || !source_rid_) {
            detail::close_message_array (parts_, part_count_);
            return;
        }

        completion_control_handler_t handler;
        {
            std::lock_guard<std::mutex> lock (
              callback_state->completion_control_handler_mutex);
            handler = callback_state->completion_control_handler;
        }
        if (!handler) {
            detail::close_message_array (parts_, part_count_);
            return;
        }

        routing_id_t source = detail::native_routing_id (*source_rid_);
        std::vector<message_t> parts =
          detail::take_parts_from_native (parts_, part_count_);
        handler (source, std::move (parts));
    };

    // The native trampoline is stable for the socket lifetime. Replacement
    // only publishes a new callable, so it cannot leave Core referring to a
    // discarded callable when registration fails.
    std::lock_guard<std::mutex> lock (state.completion_control_handler_mutex);
    if (!state.completion_control_handler_registered) {
        const zlink_handler_result_t rc =
          zlink_router_completion_control_handler (
            detail::native_handle (*this),
            static_cast<zlink_completion_control_handler_fn> (+trampoline),
            &state);
        if (rc != ZLINK_HANDLER_OK)
            throw handler_error_t (static_cast<handler_result_t> (rc),
                                   zlink_errno ());
        state.completion_control_handler_registered = true;
    }
    state.completion_control_handler = std::move (handler_);
}

} // namespace zlink
