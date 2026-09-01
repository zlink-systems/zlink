/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "core/pipe.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
typedef std::unordered_map<uint64_t, reqrep::pending_request_t>
  pending_request_map_t;

bool pending_identity_matches (
  const reqrep::pending_request_t &pending_,
  const reqrep::pending_request_identity_t &identity_)
{
    return pending_.identity == identity_;
}

uint64_t allocate_pending_cookie_locked (
  reqrep::socket_request_reply_state_t *state_)
{
    uint64_t cookie = state_->next_pending_cookie;
    if (cookie == 0)
        cookie = 1;
    state_->next_pending_cookie = cookie + 1;
    if (state_->next_pending_cookie == 0)
        state_->next_pending_cookie = 1;
    return cookie;
}

bool take_pending_request (pending_request_map_t *pending_requests_,
                           pending_request_map_t::iterator pending_,
                           reqrep::pending_request_t *pending_out_)
{
    if (!pending_requests_ || pending_ == pending_requests_->end ())
        return false;
    if (pending_out_)
        *pending_out_ = std::move (pending_->second);
    pending_requests_->erase (pending_);
    return true;
}
}

int reqrep::add_socket_pending_request_locked (
  reqrep::socket_request_reply_state_t *state_, reqrep::pending_request_t pending_)
{
    if (!state_ || pending_.identity.request_seq == 0
        || pending_.identity.cookie == 0) {
        errno = EFAULT;
        return -1;
    }

    const uint64_t request_seq = pending_.identity.request_seq;
    bool inserted = false;
    try {
#ifdef ZLINK_BUILD_TESTS
        reqrep::test_throw_request_reply_allocation_failpoint (
          reqrep::request_reply_allocation_pending_insert);
#endif
        inserted = state_->pending_requests
                     .emplace (request_seq, std::move (pending_))
                     .second;
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    if (!inserted) {
        errno = EALREADY;
        return -1;
    }
    return 0;
}

bool reqrep::remove_socket_pending_request_locked (
  reqrep::socket_request_reply_state_t *state_,
  const reqrep::pending_request_identity_t &identity_,
  reqrep::pending_request_t *pending_out_)
{
    if (!state_ || identity_.request_seq == 0 || identity_.cookie == 0)
        return false;

    pending_request_map_t::iterator pending =
      state_->pending_requests.find (identity_.request_seq);
    if (pending == state_->pending_requests.end ()
        || !pending_identity_matches (pending->second, identity_))
        return false;
    return take_pending_request (&state_->pending_requests, pending,
                                 pending_out_);
}

bool reqrep::take_pending_reply_from_transport_locked (
  reqrep::socket_request_reply_state_t *state_,
  uint64_t request_seq_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  reqrep::pending_request_t *pending_out_)
{
    if (!state_ || request_seq_ == 0)
        return false;

    pending_request_map_t::iterator pending =
      state_->pending_requests.find (request_seq_);
    if (pending == state_->pending_requests.end ())
        return false;

    // A ROUTER request may be addressed by an intended RID while the reply
    // carries the peer's settled RID. The socket-unique sequence selects the
    // aggregate; the physical transport pair remains the stale-peer fence.
    if (pending->second.transport_pair_id != transport_pair_id_
        || pending->second.transport_pair_generation
             != transport_pair_generation_)
        return false;
    return take_pending_request (&state_->pending_requests, pending,
                                 pending_out_);
}

bool reqrep::take_next_socket_pending_request_for_logical_endpoint_locked (
  reqrep::socket_request_reply_state_t *state_,
  const std::string &logical_endpoint_,
  reqrep::pending_request_t *pending_out_)
{
    if (!state_ || logical_endpoint_.empty ())
        return false;
    for (pending_request_map_t::iterator pending =
           state_->pending_requests.begin ();
         pending != state_->pending_requests.end (); ++pending) {
        if (pending->second.logical_endpoint == logical_endpoint_)
            return take_pending_request (&state_->pending_requests, pending,
                                         pending_out_);
    }
    return false;
}

bool reqrep::take_next_socket_pending_request_for_logical_rid_locked (
  reqrep::socket_request_reply_state_t *state_,
  const zlink_routing_id_t *logical_rid_,
  reqrep::pending_request_t *pending_out_)
{
    if (!state_ || !logical_rid_ || logical_rid_->size == 0)
        return false;
    for (pending_request_map_t::iterator pending =
           state_->pending_requests.begin ();
         pending != state_->pending_requests.end (); ++pending) {
        if (pending->second.logical_rid.size () == logical_rid_->size
            && memcmp (pending->second.logical_rid.data (), logical_rid_->data,
                       logical_rid_->size)
                 == 0)
            return take_pending_request (&state_->pending_requests, pending,
                                         pending_out_);
    }
    return false;
}

bool reqrep::take_next_socket_pending_request_locked (
  reqrep::socket_request_reply_state_t *state_,
  reqrep::pending_request_t *pending_out_)
{
    return state_ && !state_->pending_requests.empty ()
           && take_pending_request (&state_->pending_requests,
                                    state_->pending_requests.begin (),
                                    pending_out_);
}

int reqrep::lookup_socket_pending_request (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_,
  reqrep::pending_request_token_t *token_out_)
{
    if (!state_ || !token_out_ || identity_.request_seq == 0
        || identity_.cookie == 0) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    pending_request_map_t::const_iterator pending =
      state_->pending_requests.find (identity_.request_seq);
    if (pending == state_->pending_requests.end ()
        || !pending_identity_matches (pending->second, identity_)) {
        errno = EINVAL;
        return -1;
    }

    reqrep::pending_request_token_t token;
    token.identity = pending->second.identity;
    token.resolved_timeout_ms = pending->second.resolved_timeout_ms;
    *token_out_ = token;
    return 0;
}

bool reqrep::erase_socket_pending_request (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_)
{
    if (!state_)
        return false;

    reqrep::pending_request_t pending;
    if (reqrep::remove_socket_pending_request (state_, identity_, &pending)) {
        reqrep::release_socket_pending_request_correlation (&pending);
        zlink::request_timeout::cancel (pending.timeout_task);
        reqrep::release_pending_request_completion (state_, &pending);
        return true;
    }
    return false;
}

bool reqrep::record_socket_pending_transport_pair_identity (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_)
{
    if (!state_ || identity_.request_seq == 0 || identity_.cookie == 0
        || transport_pair_id_ == 0
        || transport_pair_generation_ == 0)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    pending_request_map_t::iterator pending =
      state_->pending_requests.find (identity_.request_seq);
    if (pending == state_->pending_requests.end ()
        || !pending_identity_matches (pending->second, identity_))
        return false;
    pending->second.transport_pair_id = transport_pair_id_;
    pending->second.transport_pair_generation = transport_pair_generation_;
    return true;
}

int reqrep::ensure_socket_pull_pending_request (
  const socket_handle_t &handle_, uint32_t timeout_ms_,
  const zlink_routing_id_t *peer_rid_, void *user_context_,
  uint64_t *request_seq_out_,
  std::shared_ptr<reqrep::socket_request_reply_state_t> *state_out_,
  reqrep::pending_request_token_t *token_out_,
  zlink_completion_id_t *completion_id_out_)
{
    if (!request_seq_out_ || !state_out_ || !token_out_
        || !completion_id_out_) {
        errno = EFAULT;
        return -1;
    }
    *request_seq_out_ = 0;
    *completion_id_out_ = 0;

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle_);
    if (!state || !handle_.socket
        || handle_.socket->ensure_completion_processing () != 0)
        return -1;

    zlink::socket_completion::reservation_t *reservation = NULL;
    zlink_completion_id_t completion_id = 0;
    if (zlink::socket_completion::reserve (
          &handle_.socket->completion_runtime (), ZLINK_COMPLETION_REQUEST,
          user_context_, peer_rid_, &reservation, &completion_id)
        != 0)
        return -1;

    reqrep::pending_request_identity_t identity;
    reqrep::pending_request_t pending;
    uint32_t resolved_timeout_ms = 0;
    int prepare_errno = 0;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        try {
            const uint64_t request_seq =
              zlink::request_reply_runtime::allocate_request_sequence (
                state.get ());
            if (request_seq == 0) {
                prepare_errno = errno;
            } else {
                identity.request_seq = request_seq;
                identity.cookie = allocate_pending_cookie_locked (state.get ());
                pending.identity = identity;
                if (peer_rid_ && peer_rid_->size != 0)
                    pending.logical_rid.assign (
                      reinterpret_cast<const char *> (peer_rid_->data),
                      peer_rid_->size);
                pending.transport_pair_id = 0;
                pending.transport_pair_generation = 0;
                resolved_timeout_ms = zlink::request_reply::resolve_timeout_ms (
                  timeout_ms_, state->default_timeout_ms);
                pending.resolved_timeout_ms = resolved_timeout_ms;
                pending.pull_completion = reservation;
                if (reqrep::add_socket_pending_request_locked (
                      state.get (), std::move (pending))
                    != 0)
                    prepare_errno = errno;
                else
                    *request_seq_out_ = request_seq;
            }
        } catch (...) {
            prepare_errno = ENOMEM;
        }
    }
    if (prepare_errno != 0) {
        zlink::socket_completion::release (
          &handle_.socket->completion_runtime (), reservation);
        errno = prepare_errno;
        return -1;
    }

    *state_out_ = state;
    token_out_->identity = identity;
    token_out_->resolved_timeout_ms = resolved_timeout_ms;
    *completion_id_out_ = completion_id;
    errno = 0;
    return 0;
}

int reqrep::arm_socket_pending_request_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const reqrep::pending_request_token_t &token_)
{
    if (!state_ || token_.identity.request_seq == 0
        || token_.identity.cookie == 0) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<zlink::request_timeout::task_t> task;
    if (reqrep::schedule_socket_pending_timeout (
          state_, token_.identity, token_.resolved_timeout_ms, &task)
        != 0) {
        // The request record is already physically committed when this is
        // called, so timeout-allocation failure cannot be reported as a
        // submit failure: the caller's payload has transferred and the peer
        // may receive it. Resolve the admitted request through its normal
        // completion channel instead.
        reqrep::pending_request_t pending;
        bool removed = false;
        {
            // Another arm may already own the live timeout. Its later schedule
            // failure must not resolve this admitted request as an error.
            std::lock_guard<std::mutex> lock (state_->mutex);
            pending_request_map_t::const_iterator current =
              state_->pending_requests.find (token_.identity.request_seq);
            if (current != state_->pending_requests.end ()
                && pending_identity_matches (current->second, token_.identity)
                && !current->second.timeout_task) {
                removed = reqrep::remove_socket_pending_request_locked (
                  state_.get (), token_.identity, &pending);
            }
        }
        if (removed) {
            reqrep::release_socket_pending_request_correlation (&pending);
            (void) reqrep::publish_pending_request_completion (
              state_, &pending, ZLINK_REQUEST_INTERNAL_ERROR, NULL, 0);
        }
        errno = 0;
        return 0;
    }

    bool installed = false;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        pending_request_map_t::iterator pending =
          state_->pending_requests.find (token_.identity.request_seq);
        if (pending != state_->pending_requests.end ()
            && pending_identity_matches (pending->second, token_.identity)
            && !pending->second.timeout_task) {
            pending->second.timeout_task = task;
            installed = true;
        }
    }
    if (!installed && task)
        zlink::request_timeout::cancel (task);
    return 0;
}
