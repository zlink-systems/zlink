/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/request_reply_frame_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
router_recv_metadata_tls_t &router_recv_metadata_tls ()
{
    static thread_local router_recv_metadata_tls_t metadata;
    return metadata;
}

namespace
{
enum completion_message_result_t
{
    completion_message_accepted,
    completion_message_protocol_error
};

completion_message_result_t complete_reply_from_transport (
  socket_request_reply_state_t *state_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  uint8_t message_type_,
  uint64_t request_sequence_,
  zlink_msg_t *parts_,
  size_t part_count_)
{
    if (!parts_ || part_count_ == 0 || request_sequence_ == 0
        || (message_type_ != zlink::request_reply::reply_type
            && message_type_ != zlink::request_reply::error_reply_type)) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return completion_message_protocol_error;
    }

    // The sequence is now owned by the pending lookup. Callback-visible
    // payload must not retain transport metadata.
    zlink::request_reply::clear_request_reply_metadata (&parts_[0]);
    if (!state_) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return completion_message_accepted;
    }

    zlink::socket_callback_scope_t callback_scope (state_->socket);
    if (!callback_scope.acquired ()) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return completion_message_accepted;
    }

    pending_request_t pending;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (!take_pending_reply_from_transport_locked (
              state_, request_sequence_, transport_pair_id_,
              transport_pair_generation_, &pending)) {
            zlink::request_reply::close_request_reply_parts (parts_, part_count_);
            return completion_message_accepted;
        }
    }
    zlink::request_timeout::cancel (pending.timeout_task);

    int callback_errno = 0;
    zlink_msg_t *callback_parts = parts_;
    size_t callback_part_count = part_count_;
    if (zlink::request_reply::decode_reply_completion (
          message_type_, parts_, part_count_,
          &callback_errno, &callback_parts, &callback_part_count)
        != 0) {
        callback_errno = EPROTO;
        callback_parts = NULL;
        callback_part_count = 0;
    }
    zlink::request_completion::claim_owner_thread (&state_->completion);
    zlink::request_completion::invoke_callback (
      state_->socket, pending.handler, callback_errno, callback_parts,
      callback_part_count, pending.userdata);
    zlink::request_completion::release_reservation (&state_->completion);
    state_->socket->notify_request_completion ();

    // Callback-visible message ownership transfers to the callback.  Do not
    // close those elements again after it returns: a conforming callback has
    // already closed, moved, or adopted each one.  ERROR_REPLY keeps its
    // leading errno frame inside Core, while a malformed completion exposes no
    // payload and therefore leaves every element Core-owned.
    size_t callback_begin = part_count_;
    size_t callback_end = part_count_;
    if (callback_parts && callback_part_count > 0) {
        callback_begin = static_cast<size_t> (callback_parts - parts_);
        callback_end = callback_begin + callback_part_count;
        zlink_assert (callback_begin <= part_count_);
        zlink_assert (callback_end <= part_count_);
    }
    // A normal reply transfers every part to the callback. Avoid walking the
    // whole multipart only to skip every element on the steady-state path.
    if (callback_begin == 0 && callback_end == part_count_)
        return completion_message_accepted;
    for (size_t i = 0; i < part_count_; ++i) {
        if (i >= callback_begin && i < callback_end)
            continue;
        zlink::request_reply::consume_send_frame (&parts_[i]);
    }
    return completion_message_accepted;
}

void discard_completion_message_tail (zlink::pipe_t *pipe_, bool more_)
{
    while (more_) {
        zlink::msg_t frame;
        const int init_rc = frame.init ();
        errno_assert (init_rc == 0);
        if (!pipe_->read (&frame)) {
            const int close_rc = frame.close ();
            errno_assert (close_rc == 0);
            return;
        }
        more_ = (frame.flags () & zlink::msg_t::more) != 0;
        const int close_rc = frame.close ();
        errno_assert (close_rc == 0);
    }
}

}

void process_completion_pipe (zlink::socket_base_t *socket_, zlink::pipe_t *pipe_)
{
    if (!socket_ || !pipe_)
        return;

    std::shared_ptr<socket_request_reply_state_t> state = socket_->request_reply_state ();
    request_reply_frame_buffer_t parts;
    while (true) {
        // Every exit below closes or consumes the current elements. Keep the
        // vector's storage for the lifetime of this drain so steady-state
        // completions do not allocate once the common part capacity is warm.
        parts.clear ();
        bool complete = false;
        bool flow_state_consumed = false;
        bool completion_delivered_directly = false;
        bool allocation_failed = false;
        uint8_t message_type = zlink::zmp_kind_data;
        uint64_t request_sequence = 0;
        while (!complete) {
            zlink::msg_t frame;
            const int init_rc = frame.init ();
            errno_assert (init_rc == 0);
            if (!pipe_->read (&frame)) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                const bool truncated_message = !parts.empty ();
                close_request_reply_frame_buffer (&parts);
                if (truncated_message)
                    pipe_->terminate (false);
                return;
            }

            const unsigned char frame_flags = frame.flags ();
            const bool more = (frame_flags & zlink::msg_t::more) != 0;

            // Core-internal flow state is a standalone completion-lane frame.
            // It is handled only at a message boundary, before application
            // kind dispatch. A command in the middle of a reply is a protocol
            // error and must not truncate that reply into a valid completion.
            if ((frame_flags & zlink::msg_t::command) != 0) {
                if (parts.empty ()
                    && socket_->consume_receive_flow_state_frame (
                         pipe_, frame)) {
                    const int close_rc = frame.close ();
                    errno_assert (close_rc == 0);
                    if (more) {
                        discard_completion_message_tail (pipe_, true);
                        close_request_reply_frame_buffer (&parts);
                        pipe_->terminate (false);
                        return;
                    }
                    flow_state_consumed = true;
                    complete = true;
                    continue;
                }

                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                discard_completion_message_tail (pipe_, more);
                close_request_reply_frame_buffer (&parts);
                pipe_->terminate (false);
                return;
            }

            if (parts.empty ()) {
                if (!zlink::request_reply::read_request_reply_metadata (
                      reinterpret_cast<const zlink_msg_t *> (&frame),
                      &message_type, &request_sequence)
                    || request_sequence == 0
                    || (message_type != zlink::request_reply::reply_type
                        && message_type != zlink::request_reply::error_reply_type)) {
                    const int close_rc = frame.close ();
                    errno_assert (close_rc == 0);
                    discard_completion_message_tail (pipe_, more);
                    close_request_reply_frame_buffer (&parts);
                    pipe_->terminate (false);
                    return;
                }

                if (!more) {
                    if (complete_reply_from_transport (
                          state.get (), pipe_->get_transport_pair_id (),
                          pipe_->get_transport_pair_generation (),
                          message_type, request_sequence,
                          reinterpret_cast<zlink_msg_t *> (&frame), 1)
                        == completion_message_protocol_error) {
                        pipe_->terminate (false);
                        return;
                    }
                    completion_delivered_directly = true;
                    complete = true;
                    continue;
                }
            } else {
                uint8_t later_kind = zlink::zmp_kind_data;
                uint64_t later_sequence = 0;
                if (zlink::request_reply::read_request_reply_metadata (
                      reinterpret_cast<const zlink_msg_t *> (&frame),
                      &later_kind, &later_sequence)) {
                    const int close_rc = frame.close ();
                    errno_assert (close_rc == 0);
                    discard_completion_message_tail (pipe_, more);
                    close_request_reply_frame_buffer (&parts);
                    pipe_->terminate (false);
                    return;
                }
            }

            try {
                parts.append_uninitialized ();
            } catch (...) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                discard_completion_message_tail (pipe_, more);
                complete = true;
                allocation_failed = true;
                break;
            }
            zlink_msg_init (&parts.back ());
            zlink::msg_t *stored = reinterpret_cast<zlink::msg_t *> (&parts.back ());
            const int move_rc = stored->move (frame);
            errno_assert (move_rc == 0);
            complete = !more;
        }

        if (flow_state_consumed)
            continue;
        if (completion_delivered_directly)
            continue;
        if (allocation_failed) {
            close_request_reply_frame_buffer (&parts);
            continue;
        }

        if (complete_reply_from_transport (
              state.get (), pipe_->get_transport_pair_id (),
              pipe_->get_transport_pair_generation (), message_type,
              request_sequence, &parts[0],
              parts.size ())
            == completion_message_protocol_error) {
            pipe_->terminate (false);
            return;
        }
    }
}

bool has_pending_request_work (const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_)
        return false;
    if (has_pending_reply_completions (state_))
        return true;

    std::lock_guard<std::mutex> lock (state_->mutex);
    return !state_->pending_requests.empty ();
}

void fail_disconnected_peer_requests (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  int errnum_)
{
    if (!state_)
        return;

    LIBZLINK_UNUSED (routing_id_);
    LIBZLINK_UNUSED (routing_id_size_);
    while (true) {
        pending_request_t failed;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            found = take_disconnected_socket_pending_request_locked (
              state_.get (), transport_pair_id_, transport_pair_generation_,
              &failed);
        }
        if (!found)
            break;
        zlink::request_timeout::cancel (failed.timeout_task);
        (void) queue_reply_completion (
          state_, failed.handler, failed.userdata, errnum_, NULL, 0);
    }
}

int drain_close_request_reply_socket (const socket_handle_t &handle_)
{
    if (!handle_.socket) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<socket_request_reply_state_t> state = handle_.socket->request_reply_state ();
    if (!state)
        return 0;

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->closing = true;
    }

    while (true) {
        pending_request_t pending;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            found = take_next_socket_pending_request_locked (state.get (),
                                                             &pending);
        }
        if (!found)
            break;
        zlink::request_timeout::cancel (pending.timeout_task);
        if (queue_reply_completion (state, pending.handler, pending.userdata,
                                    ETERM, NULL, 0)
            != 0)
            return -1;
    }

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->dealer_reply_targets.clear ();
        clear_router_reply_targets_locked (state.get ());
        state->reply_target_slots =
          state->reply_target_reservations + state->reply_target_checkouts;
    }

    return drain_reply_completions_while_closing (state, handle_.socket);
}

void cleanup_request_reply_socket (const socket_handle_t &handle_)
{
    if (!handle_.socket)
        return;

    std::shared_ptr<socket_request_reply_state_t> state = handle_.socket->request_reply_state ();
    if (state) {
        while (true) {
            pending_request_t pending;
            bool found = false;
            {
                std::lock_guard<std::mutex> state_lock (state->mutex);
                state->closing = true;
                found = take_next_socket_pending_request_locked (state.get (),
                                                                 &pending);
            }
            if (!found)
                break;
            zlink::request_timeout::cancel (pending.timeout_task);
            zlink::request_completion::release_reservation (
              &state->completion);
        }
        {
            std::lock_guard<std::mutex> state_lock (state->mutex);
            state->closing = true;
            state->dealer_reply_targets.clear ();
            clear_router_reply_targets_locked (state.get ());
            state->reply_target_slots =
              state->reply_target_reservations + state->reply_target_checkouts;
            zlink::request_completion::close (&state->completion);
        }
    }
    handle_.socket->clear_request_reply_state ();
}
}
}
