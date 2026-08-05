/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "core/pipe.hpp"
#include "utils/routing_id.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

int reqrep::lookup_socket_pending_request_by_seq (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  uint64_t request_seq_,
  reqrep::pending_key_t *key_out_)
{
    if (!state_ || !key_out_ || request_seq_ == 0) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    std::unordered_map<uint64_t, reqrep::pending_key_t>::const_iterator it =
      state_->pending_request_keys_by_seq.find (request_seq_);
    if (it == state_->pending_request_keys_by_seq.end ()) {
        errno = EINVAL;
        return -1;
    }

    *key_out_ = it->second;
    return 0;
}

bool reqrep::erase_socket_pending_request (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_key_t &key_)
{
    if (!state_)
        return false;

    reqrep::pending_request_t pending;
    if (reqrep::remove_socket_pending_request (state_, key_, &pending)) {
        zlink::request_timeout::cancel (pending.timeout_task);
        zlink::request_completion::release_reservation (&state_->completion);
        return true;
    }
    return false;
}

void reqrep::record_socket_pending_transport_pair (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_key_t &key_,
  zlink::pipe_t *transport_pair_pipe_)
{
    if (!state_ || !transport_pair_pipe_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    std::unordered_map<reqrep::pending_key_t, reqrep::pending_request_t,
                       reqrep::pending_key_hash_t>::iterator it =
      state_->pending_requests.find (key_);
    if (it == state_->pending_requests.end ())
        return;
    it->second.transport_pair_id = transport_pair_pipe_->get_transport_pair_id ();
    it->second.transport_pair_generation =
      transport_pair_pipe_->get_transport_pair_generation ();
}

int reqrep::ensure_socket_pending_request (
  socket_handle_t handle_,
  const zlink_routing_id_t *peer_rid_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_,
  uint64_t *request_seq_out_,
  std::shared_ptr<reqrep::socket_request_reply_state_t> *state_out_,
  reqrep::pending_key_t *key_out_)
{
    if (!request_seq_out_ || !state_out_ || !key_out_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle_);
    if (!state || !handle_.socket
        || handle_.socket->ensure_completion_processing () != 0)
        return -1;
    if (!zlink::request_completion::try_reserve (&state->completion))
        return -1;

    reqrep::pending_key_t key;
    reqrep::pending_request_t pending;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        const uint64_t request_seq =
          zlink::request_reply_runtime::allocate_request_sequence (state.get ());
        if (request_seq == 0)
        {
            zlink::request_completion::release_reservation (&state->completion);
            return -1;
        }

        key.request_seq = request_seq;
        if (handle_.socket->socket_type () == ZLINK_CORE_SOCKET_ROUTER
            && zlink::valid_routing_id (peer_rid_)) {
            key.peer_rid = zlink::routing_id_key (peer_rid_);
        }

        pending.key = key;
        pending.transport_pair_id = 0;
        pending.transport_pair_generation = 0;
        pending.handler = handler_;
        pending.userdata = userdata_;
        const uint32_t resolved_timeout_ms =
          zlink::request_reply::resolve_timeout_ms (timeout_ms_, state->default_timeout_ms);
        if (reqrep::schedule_socket_pending_timeout (state, key, resolved_timeout_ms,
                                                     &pending.timeout_task)
            != 0) {
            zlink::request_completion::release_reservation (&state->completion);
            return -1;
        }

        reqrep::add_socket_pending_request_locked (state.get (), key, pending);
        *request_seq_out_ = request_seq;
    }

    *state_out_ = state;
    *key_out_ = key;
    return 0;
}
