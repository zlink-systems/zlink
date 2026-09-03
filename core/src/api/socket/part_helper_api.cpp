/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <cstdio>
#include <new>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/msg.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/routing_id.hpp"

namespace
{
const bool routed_part_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTED_PART_DEBUG");

bool send_family_requires_routed_scope (zlink::part_helper_internal::send_family_t family_)
{
    using namespace zlink::part_helper_internal;
    return family_ == send_family_send_rid || family_ == send_family_router_request
           || family_ == send_family_dealer_request || family_ == send_family_dealer_request_frame
           || family_ == send_family_router_reply
           || family_ == send_family_dealer_reply;
}

bool create_send_scope (
  zlink::socket_base_t *sink_socket_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_,
  std::optional<zlink::socket_public_send_scope_t> *scope_out_)
{
    if (!sink_socket_ || !scope_out_) {
        errno = EFAULT;
        return false;
    }

    const bool needs_sync = send_family_requires_routed_scope (spec_.family);
    return sink_socket_->begin_public_send_scope (needs_sync, scope_out_);
}

bool prepare_send_scope_for_rollback (
  zlink::part_helper_internal::send_sequence_state_t *state_)
{
    if (!state_ || !state_->send_scope)
        return true;
    if (state_->send_scope->acquired ()
        || state_->send_scope->resume_multipart_call ())
        return true;

    // Close can win while a multipart marker is parked between public part
    // calls. The accepted close owns cleanup, so it may take only the raw sync
    // bit and roll the staged prefix back without re-entering public lifecycle.
    return state_->send_scope->lock_multipart_for_close_cleanup ();
}

void assign_send_sequence_spec (
  zlink::part_helper_internal::send_sequence_spec_t *destination_,
  const zlink::part_helper_internal::send_sequence_spec_t &source_)
{
    destination_->family = source_.family;
    destination_->flags = source_.flags;
    destination_->timeout_ms = source_.timeout_ms;
    destination_->request_seq = source_.request_seq;
    destination_->pending_cookie = source_.pending_cookie;
    destination_->has_rid1 = source_.has_rid1;
    destination_->has_rid2 = source_.has_rid2;
    if (source_.has_rid1)
        zlink::part_helper_internal::copy_routing_id (&source_.rid1,
                                                       &destination_->rid1);
    if (source_.has_rid2)
        zlink::part_helper_internal::copy_routing_id (&source_.rid2,
                                                       &destination_->rid2);
    destination_->has_text1 = source_.has_text1;
    destination_->has_text2 = source_.has_text2;
    if (source_.has_text1)
        destination_->text1 = source_.text1;
    else
        destination_->text1.clear ();
    if (source_.has_text2)
        destination_->text2 = source_.text2;
    else
        destination_->text2.clear ();
    destination_->request_like = source_.request_like;
    destination_->transport_pair_id = source_.transport_pair_id;
    destination_->transport_pair_generation =
      source_.transport_pair_generation;
}

bool send_spec_matches_after_request_upgrade (
  const zlink::part_helper_internal::send_sequence_spec_t &active_,
  const zlink::part_helper_internal::send_sequence_spec_t &incoming_)
{
    using zlink::part_helper_internal::routing_id_equals;
    if (active_.family != incoming_.family
        || active_.flags != incoming_.flags
        || active_.has_rid1 != incoming_.has_rid1
        || active_.has_rid2 != incoming_.has_rid2
        || active_.has_text1 != incoming_.has_text1
        || active_.has_text2 != incoming_.has_text2
        || active_.request_like != incoming_.request_like
        || active_.transport_pair_id != incoming_.transport_pair_id
        || active_.transport_pair_generation
             != incoming_.transport_pair_generation)
        return false;
    if (active_.has_rid1
        && !routing_id_equals (active_.rid1, incoming_.rid1))
        return false;
    if (active_.has_rid2
        && !routing_id_equals (active_.rid2, incoming_.rid2))
        return false;
    if (active_.has_text1 && active_.text1 != incoming_.text1)
        return false;
    if (active_.has_text2 && active_.text2 != incoming_.text2)
        return false;
    return true;
}

void clear_send_sequence_spec (
  zlink::part_helper_internal::send_sequence_spec_t *spec_)
{
    spec_->family = zlink::part_helper_internal::send_family_none;
    spec_->flags = ZLINK_SEND_FLAGS_NONE;
    spec_->timeout_ms = 0;
    spec_->request_seq = 0;
    spec_->pending_cookie = 0;
    spec_->rid1.size = 0;
    spec_->rid2.size = 0;
    spec_->has_rid1 = false;
    spec_->has_rid2 = false;
    spec_->text1.clear ();
    spec_->text2.clear ();
    spec_->has_text1 = false;
    spec_->has_text2 = false;
    spec_->request_like = false;
    spec_->transport_pair_id = 0;
    spec_->transport_pair_generation = 0;
}

int prepare_send_step_state_locked (
  zlink::part_helper_internal::handle_state_t *state_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_,
  zlink::socket_base_t *sink_socket_, bool *first_part_out_,
  bool start_if_inactive_)
{
    const std::thread::id current_thread = std::this_thread::get_id ();
    bool advertised_new_sequence = false;
    if (state_->send.active && state_->send.owner_thread != current_thread) {
        if (zlink::part_helper_internal::routed_part_debug_enabled ()) {
            std::fprintf (stderr,
                          "[routed-part-debug] prepare_send_step busy "
                          "family=%d active_family=%d same_thread=0\n",
                          static_cast<int> (spec_.family),
                          static_cast<int> (state_->send.spec.family));
        }
        errno = EINVAL;
        return -1;
    }

    try {
        if (!state_->send.active) {
            if (!start_if_inactive_)
                return 1;

            std::optional<zlink::socket_public_send_scope_t> send_scope;
            // Publish the helper marker while its state mutex is still held.
            // A concurrent FINAL fast path that observes it must join the
            // helper path and block on this same mutex; publishing only after
            // create_send_scope() left a marker-to-state admission gap.
            sink_socket_->set_part_helper_send_active (true);
            advertised_new_sequence = true;
            if (!create_send_scope (sink_socket_, spec_, &send_scope)) {
                const int scope_errno = errno;
                sink_socket_->set_part_helper_send_active (false);
                // A Core-owned pending-send retry briefly uses the complete
                // admission slot.  It is backpressure, not a malformed new
                // multipart sequence.  Publish a retryable POLLOUT edge so a
                // DONTWAIT caller cannot turn that internal race into EINVAL.
                if (scope_errno == EINVAL
                    && spec_.flags == ZLINK_SEND_FLAGS_DONTWAIT) {
                    sink_socket_->arm_send_recovery_after_backpressure ();
                    errno = EAGAIN;
                } else {
                    errno = scope_errno;
                }
                return -1;
            }

            assign_send_sequence_spec (&state_->send.spec, spec_);
            state_->send.sink_socket = sink_socket_;
            state_->send.send_scope.emplace (std::move (*send_scope));
            state_->send.owner_thread = current_thread;
            state_->send.active = true;
            advertised_new_sequence = false;
            *first_part_out_ = true;
        } else {
            bool commit_upgraded_spec = false;
            if (!zlink::part_helper_internal::send_spec_equals (
                  state_->send.spec, spec_)) {
                const bool can_upgrade_staged_request =
                  state_->send.spec.request_like && spec_.request_like
                  && state_->send.spec.request_seq == 0 && spec_.request_seq != 0
                  && send_spec_matches_after_request_upgrade (
                    state_->send.spec, spec_);
                if (can_upgrade_staged_request) {
                    commit_upgraded_spec = true;
                } else {
                    if (zlink::part_helper_internal::routed_part_debug_enabled ()) {
                        std::fprintf (
                          stderr,
                          "[routed-part-debug] prepare_send_step spec "
                          "mismatch family=%d active_family=%d\n",
                          static_cast<int> (spec_.family),
                          static_cast<int> (state_->send.spec.family));
                    }
                    errno = EINVAL;
                    return -1;
                }
            }
            if (!state_->send.send_scope
                || !state_->send.send_scope->resume_multipart_call ()) {
                if (!state_->send.send_scope)
                    errno = EFAULT;
                return -1;
            }
            if (commit_upgraded_spec) {
                try {
                    assign_send_sequence_spec (&state_->send.spec, spec_);
                } catch (...) {
                    state_->send.send_scope->suspend_multipart_call ();
                    throw;
                }
            }
            *first_part_out_ = false;
        }
    } catch (const std::bad_alloc &) {
        if (advertised_new_sequence)
            sink_socket_->set_part_helper_send_active (false);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}
}

zlink::part_helper_internal::send_sequence_spec_t::send_sequence_spec_t () :
    family (send_family_none),
    flags (ZLINK_SEND_FLAGS_NONE),
    timeout_ms (0),
    request_seq (0),
    pending_cookie (0),
    has_rid1 (false),
    has_rid2 (false),
    has_text1 (false),
    has_text2 (false),
    request_like (false),
    transport_pair_id (0),
    transport_pair_generation (0)
{
    memset (&rid1, 0, sizeof (rid1));
    memset (&rid2, 0, sizeof (rid2));
}

zlink::part_helper_internal::send_sequence_state_t::send_sequence_state_t () :
    active (false),
    sink_socket (NULL)
{
}

zlink::part_helper_internal::recv_sequence_state_t::recv_sequence_state_t () :
    active (false),
    family (recv_family_none),
    source_socket (NULL),
    owner_thread (),
    return_source_rid_as_null (true),
    request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    message_type (0),
    subscribed (0),
    next_part_index (0),
    public_delivery_hold (false)
{
    memset (&source_node_rid, 0, sizeof (source_node_rid));
}

int zlink::part_helper_internal::validate_send_flags (zlink_send_flags_t flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zlink::part_helper_internal::adopt_recv_public_delivery_hold (
  const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (!state_->recv.active || !state_->recv.source_socket
        || state_->recv.public_delivery_hold) {
        errno = EINVAL;
        return -1;
    }
    state_->recv.public_delivery_hold = true;
    return 0;
}

int zlink::part_helper_internal::validate_part_flag (zlink_part_flag_t part_flag_)
{
    if (part_flag_ != ZLINK_PART_FINAL && part_flag_ != ZLINK_PART_MORE) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

bool zlink::part_helper_internal::routing_id_equals (const zlink_routing_id_t &lhs_,
                                                     const zlink_routing_id_t &rhs_)
{
    return lhs_.size == rhs_.size && memcmp (lhs_.data, rhs_.data, lhs_.size) == 0;
}

void zlink::part_helper_internal::copy_routing_id (const zlink_routing_id_t *src_,
                                                   zlink_routing_id_t *dest_)
{
    if (!dest_)
        return;

    if (!src_) {
        memset (dest_, 0, sizeof (*dest_));
        return;
    }

    zlink::copy_routing_id_from_bytes (src_->data, src_->size, dest_);
}

void zlink::part_helper_internal::consume_send_part (zlink_msg_t *part_)
{
    zlink::request_reply::consume_send_frame (part_);
}

bool zlink::part_helper_internal::routed_part_debug_enabled ()
{
    return routed_part_debug_on;
}

void zlink::part_helper_internal::trace_routed_part_prepare_failed (send_family_t family_,
                                                                    int err_)
{
    if (!routed_part_debug_enabled ())
        return;

    std::fprintf (stderr,
                  "[routed-part-debug] prepare_send_step failed family=%d errno=%d\n",
                  static_cast<int> (family_), err_);
}

void zlink::part_helper_internal::trace_routed_part_send_failed (send_family_t family_,
                                                                bool first_part_,
                                                                int err_)
{
    if (!routed_part_debug_enabled ())
        return;

    std::fprintf (stderr,
                  "[routed-part-debug] send_fn failed family=%d first=%d errno=%d\n",
                  static_cast<int> (family_), first_part_ ? 1 : 0, err_);
}

void zlink::part_helper_internal::trace_routed_part_first_send (
  const zlink_routing_id_t &rid_, zlink_msg_t *part_, zlink_send_flags_t flags_)
{
    if (!routed_part_debug_enabled ())
        return;

    std::fprintf (stderr,
                  "[routed-part-debug] routed send first_part "
                  "rid_size=%u msg_size=%zu flags=%d\n",
                  static_cast<unsigned> (rid_.size), zlink_msg_size (part_),
                  static_cast<int> (flags_));
}

bool zlink::part_helper_internal::send_spec_equals (const send_sequence_spec_t &lhs_,
                                                    const send_sequence_spec_t &rhs_)
{
    if (lhs_.family != rhs_.family || lhs_.flags != rhs_.flags || lhs_.timeout_ms != rhs_.timeout_ms
        || lhs_.request_seq != rhs_.request_seq
        || lhs_.pending_cookie != rhs_.pending_cookie || lhs_.has_rid1 != rhs_.has_rid1
        || lhs_.has_rid2 != rhs_.has_rid2 || lhs_.has_text1 != rhs_.has_text1
        || lhs_.has_text2 != rhs_.has_text2 || lhs_.request_like != rhs_.request_like
        || lhs_.transport_pair_id != rhs_.transport_pair_id
        || lhs_.transport_pair_generation != rhs_.transport_pair_generation) {
        return false;
    }

    if (lhs_.has_rid1 && !routing_id_equals (lhs_.rid1, rhs_.rid1))
        return false;
    if (lhs_.has_rid2 && !routing_id_equals (lhs_.rid2, rhs_.rid2))
        return false;
    if (lhs_.has_text1 && lhs_.text1 != rhs_.text1)
        return false;
    if (lhs_.has_text2 && lhs_.text2 != rhs_.text2)
        return false;

    return true;
}

bool zlink::part_helper_internal::recv_sequence_active (
  const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    return state_->recv.active;
}

bool zlink::part_helper_internal::send_sequence_active (void *handle_)
{
    std::shared_ptr<handle_state_t> state = find_handle_state (handle_);
    if (!state)
        return false;

    std::lock_guard<std::mutex> lock (state->mutex);
    return state->send.active;
}

bool zlink::part_helper_internal::send_sequence_active (socket_base_t *socket_)
{
    return socket_ && socket_->part_helper_send_active ();
}

int zlink::part_helper_internal::stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                                                      recv_family_t family_,
                                                      zlink::socket_base_t *source_socket_,
                                                      const zlink_routing_id_t *source_node_rid_,
                                                      uint64_t request_seq_,
                                                      zlink_msg_t *parts_,
                                                      size_t part_count_,
                                                      std::thread::id owner_thread_)
{
    if (!state_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->recv.active) {
        errno = EINVAL;
        return -1;
    }

    state_->recv.active = true;
    state_->recv.family = family_;
    state_->recv.source_socket = source_socket_;
    state_->recv.owner_thread = owner_thread_;
    set_recv_metadata (&state_->recv, source_node_rid_, request_seq_);
    if (buffer_recv_parts (&state_->recv, parts_, part_count_) != 0) {
        const int saved_errno = errno;
        reset_recv_sequence (&state_->recv);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

void zlink::part_helper_internal::set_recv_metadata (recv_sequence_state_t *recv_,
                                                     const zlink_routing_id_t *source_node_rid_,
                                                     uint64_t request_seq_)
{
    if (!recv_)
        return;

    recv_->return_source_rid_as_null = source_node_rid_ == NULL;
    copy_routing_id (source_node_rid_, &recv_->source_node_rid);
    recv_->request_seq = request_seq_;
    recv_->message_type = 0;
}

void zlink::part_helper_internal::set_recv_transport_pair (
  recv_sequence_state_t *recv_, uint64_t transport_pair_id_, uint64_t transport_pair_generation_)
{
    if (!recv_)
        return;
    recv_->transport_pair_id = transport_pair_id_;
    recv_->transport_pair_generation = transport_pair_generation_;
}

int zlink::part_helper_internal::buffer_recv_parts (recv_sequence_state_t *recv_,
                                                    zlink_msg_t *parts_,
                                                    size_t part_count_)
{
    if (!recv_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    try {
        recv_->buffered_parts.resize (part_count_);
    } catch (...) {
        recv_->next_part_index = 0;
        errno = ENOMEM;
        return -1;
    }
    recv_->next_part_index = 0;
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_init (&recv_->buffered_parts[i]);

    for (size_t i = 0; i < part_count_; ++i) {
        if (zlink_msg_move (&recv_->buffered_parts[i], &parts_[i]) != 0) {
            for (size_t j = 0; j < recv_->buffered_parts.size (); ++j)
                zlink_msg_close (&recv_->buffered_parts[j]);
            recv_->buffered_parts.clear ();
            recv_->next_part_index = 0;
            errno = EFAULT;
            return -1;
        }
    }

    return 0;
}

int zlink::part_helper_internal::take_recv_part (recv_sequence_state_t *recv_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    if (!recv_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return -1;
    }
    if (recv_->next_part_index >= recv_->buffered_parts.size ()) {
        errno = EPROTO;
        return -1;
    }
    if (zlink_msg_move (part_out_, &recv_->buffered_parts[recv_->next_part_index]) != 0) {
        errno = EFAULT;
        return -1;
    }
    ++recv_->next_part_index;
    *has_more_out_ =
      recv_->next_part_index < recv_->buffered_parts.size () ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
    return 0;
}

int zlink::part_helper_internal::take_recv_part (const std::shared_ptr<handle_state_t> &state_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    return take_recv_part (&state_->recv, part_out_, has_more_out_);
}

int zlink::part_helper_internal::take_recv_part (
  const std::shared_ptr<handle_state_t> &state_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (take_recv_part (&state_->recv, part_out_, has_more_out_) != 0)
        return -1;
    if (source_node_rid_out_) {
        *source_node_rid_out_ = state_->recv.return_source_rid_as_null
                                  ? NULL
                                  : &state_->recv.source_node_rid;
    }
    if (request_seq_out_)
        *request_seq_out_ = state_->recv.request_seq;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = state_->recv.transport_pair_id;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ =
          state_->recv.transport_pair_generation;
    return 0;
}

void zlink::part_helper_internal::export_recv_metadata (
  const std::shared_ptr<handle_state_t> &state_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (source_node_rid_out_) {
        *source_node_rid_out_ =
          state_->recv.return_source_rid_as_null ? NULL : &state_->recv.source_node_rid;
    }
    if (request_seq_out_)
        *request_seq_out_ = state_->recv.request_seq;
}

void zlink::part_helper_internal::reset_send_sequence (
  send_sequence_state_t *state_, bool notify_release_)
{
    if (!state_)
        return;

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();

    socket_base_t *const sink_socket = state_->sink_socket;
    const bool had_send_scope = static_cast<bool> (state_->send_scope);

    clear_send_sequence_spec (&state_->spec);
    state_->send_scope.reset ();
    state_->active = false;
    state_->sink_socket = NULL;
    state_->owner_thread = std::thread::id ();

    if (sink_socket)
        sink_socket->set_part_helper_send_active (false);

    if (notify_release_ && had_send_scope && sink_socket)
        sink_socket->notify_incremental_send_released ();
}

zlink::socket_base_t *zlink::part_helper_internal::reset_recv_sequence (
  recv_sequence_state_t *state_)
{
    if (!state_)
        return NULL;

    socket_base_t *const held_socket =
      state_->public_delivery_hold ? state_->source_socket : NULL;

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();
    state_->next_part_index = 0;

    state_->active = false;
    state_->family = recv_family_none;
    state_->source_socket = NULL;
    state_->owner_thread = std::thread::id ();
    state_->return_source_rid_as_null = true;
    copy_routing_id (NULL, &state_->source_node_rid);
    state_->request_seq = 0;
    state_->transport_pair_id = 0;
    state_->transport_pair_generation = 0;
    state_->message_type = 0;
    state_->subscribed = 0;
    state_->topic_id.clear ();
    state_->public_delivery_hold = false;
    return held_socket;
}

int zlink::part_helper_internal::prepare_send_step (void *handle_,
                                                    const send_sequence_spec_t &spec_,
                                                    zlink::socket_base_t *sink_socket_,
                                                    std::shared_ptr<handle_state_t> *state_out_,
                                                    bool *first_part_out_)
{
    LIBZLINK_UNUSED (handle_);

    if (!state_out_ || !first_part_out_) {
        errno = EFAULT;
        return -1;
    }

    if (!sink_socket_) {
        errno = EFAULT;
        return -1;
    }
    std::shared_ptr<handle_state_t> state = find_or_create_socket_state (sink_socket_);
    if (!state)
        return -1;

    std::lock_guard<std::mutex> lock (state->mutex);
    if (prepare_send_step_state_locked (state.get (), spec_, sink_socket_,
                                        first_part_out_, true)
        != 0)
        return -1;

    *state_out_ = state;
    return 0;
}

int zlink::part_helper_internal::prepare_send_step_locked (
  void *handle_, const send_sequence_spec_t &spec_,
  zlink::socket_base_t *sink_socket_,
  std::shared_ptr<handle_state_t> *state_out_,
  std::unique_lock<std::mutex> *lock_out_, bool *first_part_out_,
  bool start_if_inactive_)
{
    LIBZLINK_UNUSED (handle_);
    if (!state_out_ || !lock_out_ || !first_part_out_ || !sink_socket_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<handle_state_t> state =
      start_if_inactive_ ? find_or_create_socket_state (sink_socket_)
                         : find_socket_state (sink_socket_);
    if (!state)
        return start_if_inactive_ ? -1 : 1;

    std::unique_lock<std::mutex> lock (state->mutex);
    const int rc = prepare_send_step_state_locked (
      state.get (), spec_, sink_socket_, first_part_out_,
      start_if_inactive_);
    if (rc != 0)
        return rc;

    *state_out_ = state;
    *lock_out_ = std::move (lock);
    return 0;
}

int zlink::part_helper_internal::prepare_recv_step (
  void *handle_,
  recv_family_t family_,
  zlink::socket_base_t *source_socket_,
  std::shared_ptr<handle_state_t> *state_inout_,
  bool *first_part_out_,
  zlink::socket_base_t **active_source_socket_out_)
{
    if (!state_inout_ || !first_part_out_ || !active_source_socket_out_) {
        errno = EFAULT;
        return -1;
    }

    // The public entry may already own the socket lifetime and resolve its
    // socket-owned state. Reuse that operation context instead of acquiring
    // the public handle a second time. Foreign-handle callers retain the
    // registry fallback through handle_.
    std::shared_ptr<handle_state_t> state = *state_inout_;
    if (!state)
        state = find_or_create_handle_state (handle_);
    if (!state)
        return -1;

    std::lock_guard<std::mutex> lock (state->mutex);
    const std::thread::id current_thread = std::this_thread::get_id ();

    if (!state->recv.active) {
        state->recv.active = true;
        state->recv.family = family_;
        state->recv.source_socket = source_socket_;
        state->recv.owner_thread = current_thread;
        state->recv.return_source_rid_as_null = true;
        state->recv.request_seq = 0;
        state->recv.transport_pair_id = 0;
        state->recv.transport_pair_generation = 0;
        state->recv.topic_id.clear ();
        memset (&state->recv.source_node_rid, 0, sizeof (state->recv.source_node_rid));
        *first_part_out_ = true;
    } else {
        if (state->recv.family != family_ || state->recv.owner_thread != current_thread) {
            errno = EBUSY;
            return -1;
        }
        *first_part_out_ = false;
    }

    *active_source_socket_out_ = state->recv.source_socket;
    *state_inout_ = state;
    return 0;
}

void zlink::part_helper_internal::complete_send_step (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink_part_flag_t part_flag_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    complete_send_step_locked (state_.get (), part_flag_);
}

void zlink::part_helper_internal::complete_send_step_locked (
  handle_state_t *state_, zlink_part_flag_t part_flag_)
{
    if (!state_)
        return;

    if (part_flag_ == ZLINK_PART_MORE) {
        if (state_->send.sink_socket)
            state_->send.sink_socket->hold_incremental_send_control_boundary ();
        if (state_->send.send_scope)
            state_->send.send_scope->suspend_multipart_call ();
        return;
    }
    if (part_flag_ != ZLINK_PART_FINAL)
        return;

    reset_send_sequence (&state_->send);
}

int zlink::part_helper_internal::take_buffered_send_record (
  const std::shared_ptr<handle_state_t> &state_, send_part_buffer_t *parts_out_)
{
    if (!state_ || !parts_out_ || !parts_out_->empty ()) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    return take_buffered_send_record_locked (state_.get (), parts_out_);
}

int zlink::part_helper_internal::take_buffered_send_record_locked (
  handle_state_t *state_, send_part_buffer_t *parts_out_)
{
    if (!state_ || !parts_out_ || !parts_out_->empty ()) {
        errno = EFAULT;
        return -1;
    }
    if (!state_->send.active || state_->send.buffered_parts.empty ()
        || state_->send.owner_thread != std::this_thread::get_id ()) {
        errno = EINVAL;
        return -1;
    }

    parts_out_->take_from (&state_->send.buffered_parts);
    reset_send_sequence (&state_->send);
    return 0;
}

void zlink::part_helper_internal::complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink_part_flag_t has_more_)
{
    if (!state_ || has_more_ != ZLINK_PART_FINAL)
        return;

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        held_socket = reset_recv_sequence (&state_->recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
}

void zlink::part_helper_internal::abort_send_step (const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->send.sink_socket && state_->send.send_scope) {
        if (!prepare_send_scope_for_rollback (&state_->send))
            return;
        (void) state_->send.sink_socket->rollback_scoped (*state_->send.send_scope);
        reset_send_sequence (&state_->send);
        return;
    }
    reset_send_sequence (&state_->send);
}

void zlink::part_helper_internal::abort_current_non_publish_send_sequence (void *handle_)
{
    const int saved_errno = errno;
    std::shared_ptr<handle_state_t> state = find_handle_state (handle_);
    if (state) {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (state->send.active
            && state->send.owner_thread == std::this_thread::get_id ()
            && state->send.spec.family != send_family_publish) {
            if (state->send.sink_socket && state->send.send_scope
                && prepare_send_scope_for_rollback (&state->send)) {
                (void) state->send.sink_socket->rollback_scoped (
                  *state->send.send_scope);
                reset_send_sequence (&state->send);
            } else if (!state->send.sink_socket || !state->send.send_scope) {
                reset_send_sequence (&state->send);
            }
        }
    }
    errno = saved_errno;
}

void zlink::part_helper_internal::abort_recv_step (const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return;

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        held_socket = reset_recv_sequence (&state_->recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
}

int zlink::part_helper_internal::reject_if_send_sequence_open (void *handle_)
{
    std::shared_ptr<handle_state_t> state = find_handle_state (handle_);
    if (!state)
        return 0;

    std::lock_guard<std::mutex> lock (state->mutex);
    if (state->send.active) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void zlink::part_helper_internal::invalidate_recv_sequence (void *handle_)
{
    std::shared_ptr<handle_state_t> state = find_handle_state (handle_);
    if (!state)
        return;

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        held_socket = reset_recv_sequence (&state->recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
}
