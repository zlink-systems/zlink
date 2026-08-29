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
void complete_reply_from_transport (
  socket_request_reply_state_t *state_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *parts_,
  size_t part_count_)
{
    zlink::request_reply::parsed_envelope_t envelope;
    if (!state_ || !zlink::request_reply::parse_envelope (parts_, part_count_, &envelope)
        || envelope.message_type == zlink::request_reply::request_type) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return;
    }

    zlink::socket_callback_scope_t callback_scope (state_->socket);
    if (!callback_scope.acquired ()) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return;
    }

    pending_request_t pending;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (!take_pending_reply_from_transport_locked (
              state_, envelope.request_seq, transport_pair_id_,
              transport_pair_generation_, &pending)) {
            zlink::request_reply::close_request_reply_parts (parts_, part_count_);
            return;
        }
    }
    zlink::request_timeout::cancel (pending.timeout_task);

    int callback_errno = 0;
    zlink_msg_t *callback_parts = envelope.payload_parts;
    size_t callback_part_count = envelope.payload_part_count;
    if (zlink::request_reply::decode_reply_completion (
          envelope.message_type, envelope.payload_parts, envelope.payload_part_count,
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
    zlink::request_reply::close_request_reply_parts (parts_, part_count_);
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
        bool malformed = false;
        while (!complete) {
            zlink::msg_t frame;
            const int init_rc = frame.init ();
            errno_assert (init_rc == 0);
            if (!pipe_->read (&frame)) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                close_request_reply_frame_buffer (&parts);
                return;
            }

            const unsigned char frame_flags = frame.flags ();
            const bool more = (frame_flags & zlink::msg_t::more) != 0;

            //  Core-internal flow state never reaches the reply dispatcher and
            //  never becomes an application part. On a local (inproc) pair the
            //  frame is queued on the completion pipe instead of being
            //  intercepted by a session, so it is classified here as well - at
            //  any position, because a frame that is not where it belongs must
            //  still never be delivered.
            //
            //  Ordinary reply parts are not command frames, and this is the
            //  per-part path of every completion, so the command-flag test
            //  stays inline and only a command frame pays for the call.
            if ((frame_flags & zlink::msg_t::command) != 0
                && socket_->consume_receive_flow_state_frame (pipe_, frame)) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                if (!more && !parts.empty ()) {
                    //  The frame stood where this message's final part should
                    //  have been. The accumulated parts are a truncated
                    //  message, and parse_envelope does not look at more-flags,
                    //  so letting them through would complete an outstanding
                    //  request with a reply that was never sent. End the
                    //  message and reject it instead.
                    complete = true;
                    malformed = true;
                }
                continue;
            }

            try {
                parts.push_back (zlink_msg_t ());
            } catch (...) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                discard_completion_message_tail (pipe_, more);
                complete = true;
                malformed = true;
                break;
            }
            zlink_msg_init (&parts.back ());
            zlink::msg_t *stored = reinterpret_cast<zlink::msg_t *> (&parts.back ());
            const int move_rc = stored->move (frame);
            errno_assert (move_rc == 0);
            complete = !more;
        }

        if (!state || malformed) {
            close_request_reply_frame_buffer (&parts);
            continue;
        }

        complete_reply_from_transport (
          state.get (), pipe_->get_transport_pair_id (),
          pipe_->get_transport_pair_generation (), &parts[0], parts.size ());
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
        state->router_reply_targets.clear ();
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
            state->router_reply_targets.clear ();
            state->reply_target_slots =
              state->reply_target_reservations + state->reply_target_checkouts;
            zlink::request_completion::close (&state->completion);
        }
    }
    handle_.socket->clear_request_reply_state ();
}
}
}
