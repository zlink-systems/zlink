/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <vector>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

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
void close_control_envelope_parts (zlink_msg_t *parts_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < zlink::request_reply::control_part_count; ++i)
        zlink_msg_close (&parts_[i]);
}

void dispatch_completion_control (
  socket_request_reply_state_t *state_,
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *parts_,
  size_t part_count_,
  const zlink::request_reply::parsed_envelope_t &envelope_)
{
    zlink_completion_control_handler_fn handler = NULL;
    void *userdata = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        handler = state_->completion_control_handler;
        userdata = state_->completion_control_userdata;
    }

    if (!handler) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return;
    }

    zlink::socket_callback_scope_t callback_scope (state_->socket);
    if (!callback_scope.acquired ()) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return;
    }

    // The callback owns only payload frames. Core retains and closes the four
    // envelope frames after the callback returns.
    handler (source_rid_, envelope_.payload_parts,
             envelope_.payload_part_count, userdata);
    close_control_envelope_parts (parts_);
}

void complete_reply_from_transport (
  socket_request_reply_state_t *state_,
  const zlink_routing_id_t *source_rid_,
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

    if (envelope.message_type == zlink::request_reply::completion_control_type) {
        dispatch_completion_control (
          state_, source_rid_, parts_, part_count_, envelope);
        return;
    }

    zlink::socket_callback_scope_t callback_scope (state_->socket);
    if (!callback_scope.acquired ()) {
        zlink::request_reply::close_request_reply_parts (parts_, part_count_);
        return;
    }

    pending_key_t key;
    key.request_seq = envelope.request_seq;
    if (state_->socket_type == ZLINK_CORE_SOCKET_ROUTER
        && zlink::valid_routing_id (source_rid_))
        key.peer_rid = zlink::routing_id_key (source_rid_);

    pending_request_t pending;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (!take_pending_reply_from_transport_locked (
              state_, key, transport_pair_id_, transport_pair_generation_, &pending)) {
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

}

void process_completion_pipe (zlink::socket_base_t *socket_, zlink::pipe_t *pipe_)
{
    if (!socket_ || !pipe_)
        return;

    std::shared_ptr<socket_request_reply_state_t> state = socket_->request_reply_state ();
    while (true) {
        std::vector<zlink_msg_t> parts;
        bool complete = false;
        while (!complete) {
            zlink::msg_t frame;
            const int init_rc = frame.init ();
            errno_assert (init_rc == 0);
            if (!pipe_->read (&frame)) {
                const int close_rc = frame.close ();
                errno_assert (close_rc == 0);
                zlink::request_reply::close_built_parts (&parts);
                return;
            }

            parts.push_back (zlink_msg_t ());
            zlink_msg_init (&parts.back ());
            zlink::msg_t *stored = reinterpret_cast<zlink::msg_t *> (&parts.back ());
            const bool more = (frame.flags () & zlink::msg_t::more) != 0;
            const int move_rc = stored->move (frame);
            errno_assert (move_rc == 0);
            complete = !more;
        }

        if (!state) {
            zlink::request_reply::close_built_parts (&parts);
            continue;
        }

        zlink_routing_id_t source_rid;
        memset (&source_rid, 0, sizeof (source_rid));
        const blob_t *rid = &pipe_->get_routing_id ();
        if (rid->size () == 0) {
            pipe_t *application = socket_->application_pipe_for_completion (pipe_);
            if (application)
                rid = &application->get_routing_id ();
        }
        if (rid->size () > 0 && rid->size () <= sizeof (source_rid.data)) {
            source_rid.size = static_cast<uint8_t> (rid->size ());
            memcpy (source_rid.data, rid->data (), rid->size ());
        }
        complete_reply_from_transport (
          state.get (), source_rid.size > 0 ? &source_rid : NULL,
          pipe_->get_transport_pair_id (), pipe_->get_transport_pair_generation (),
          &parts[0], parts.size ());
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
    std::vector<pending_request_t> failed;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        for (std::unordered_map<pending_key_t, pending_request_t,
                                pending_key_hash_t>::iterator it =
               state_->pending_requests.begin ();
             it != state_->pending_requests.end ();) {
            const bool matches =
              it->second.transport_pair_id == transport_pair_id_
              && it->second.transport_pair_generation
                   == transport_pair_generation_;
            if (!matches) {
                ++it;
                continue;
            }
            failed.push_back (it->second);
            state_->pending_sequences.erase (it->first.request_seq);
            state_->pending_request_keys_by_seq.erase (it->first.request_seq);
            it = state_->pending_requests.erase (it);
        }
    }

    for (size_t i = 0; i < failed.size (); ++i) {
        zlink::request_timeout::cancel (failed[i].timeout_task);
        (void) queue_reply_completion (
          state_, failed[i].handler, failed[i].userdata, errnum_, NULL, 0);
    }
}

int drain_close_request_reply_socket (socket_handle_t handle_)
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

    std::vector<pending_request_t> pending;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        for (std::unordered_map<pending_key_t, pending_request_t, pending_key_hash_t>::iterator it =
               state->pending_requests.begin ();
             it != state->pending_requests.end (); ++it) {
            pending.push_back (it->second);
        }
        state->pending_requests.clear ();
        state->pending_request_keys_by_seq.clear ();
        state->pending_sequences.clear ();
        state->dealer_reply_targets.clear ();
        state->router_reply_targets.clear ();
        state->reply_target_slots = 0;
    }

    for (size_t i = 0; i < pending.size (); ++i) {
        zlink::request_timeout::cancel (pending[i].timeout_task);
        if (queue_reply_completion (state, pending[i].handler, pending[i].userdata, ETERM, NULL, 0)
            != 0) {
            return -1;
        }
    }

    return drain_reply_completions_while_closing (state, handle_.socket);
}

void cleanup_request_reply_socket (socket_handle_t handle_)
{
    if (!handle_.socket)
        return;

    std::vector<std::shared_ptr<zlink::request_timeout::task_t>> timeout_tasks;
    std::shared_ptr<socket_request_reply_state_t> state = handle_.socket->request_reply_state ();
    if (state) {
        {
            std::lock_guard<std::mutex> state_lock (state->mutex);
            state->closing = true;
            for (std::unordered_map<pending_key_t, pending_request_t,
                                    pending_key_hash_t>::iterator it =
                   state->pending_requests.begin ();
                 it != state->pending_requests.end (); ++it) {
                timeout_tasks.push_back (it->second.timeout_task);
            }
            state->pending_requests.clear ();
            state->pending_request_keys_by_seq.clear ();
            state->pending_sequences.clear ();
            state->dealer_reply_targets.clear ();
            state->router_reply_targets.clear ();
            state->reply_target_slots = 0;
            zlink::request_completion::close (&state->completion);
        }
    }
    for (size_t i = 0; i < timeout_tasks.size (); ++i)
        zlink::request_timeout::cancel (timeout_tasks[i]);
    handle_.socket->clear_request_reply_state ();
}
}
}
