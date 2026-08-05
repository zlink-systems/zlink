/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <cstdio>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/msg.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/routing_id.hpp"

namespace
{
const bool routed_part_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTED_PART_DEBUG");

bool &aggregate_send_mode_tls ()
{
    static thread_local bool active = false;
    return active;
}

bool send_family_requires_routed_scope (zlink::part_helper_internal::send_family_t family_)
{
    using namespace zlink::part_helper_internal;
    return family_ == send_family_send_rid || family_ == send_family_router_request
           || family_ == send_family_dealer_request || family_ == send_family_dealer_request_frame
           || family_ == send_family_router_reply
           || family_ == send_family_dealer_reply;
}

std::unique_ptr<zlink::socket_public_send_scope_t>
create_send_scope (zlink::socket_base_t *sink_socket_,
                   const zlink::part_helper_internal::send_sequence_spec_t &spec_)
{
    if (!sink_socket_) {
        errno = EFAULT;
        return std::unique_ptr<zlink::socket_public_send_scope_t> ();
    }

    const bool needs_sync = send_family_requires_routed_scope (spec_.family);
    return sink_socket_->begin_public_send_scope (needs_sync);
}
}

zlink::part_helper_internal::send_sequence_spec_t::send_sequence_spec_t () :
    family (send_family_none),
    flags (ZLINK_SEND_FLAGS_NONE),
    timeout_ms (0),
    request_seq (0),
    handler (NULL),
    userdata (NULL),
    has_rid1 (false),
    has_rid2 (false),
    has_text1 (false),
    has_text2 (false),
    request_like (false)
{
    memset (&rid1, 0, sizeof (rid1));
    memset (&rid2, 0, sizeof (rid2));
}

zlink::part_helper_internal::send_sequence_state_t::send_sequence_state_t () :
    active (false), sink_socket (NULL)
{
}

zlink::part_helper_internal::recv_sequence_state_t::recv_sequence_state_t () :
    active (false),
    family (recv_family_none),
    source_socket (NULL),
    owner_thread (),
    return_source_rid_as_null (true),
    request_seq (0),
    message_type (0),
    next_part_index (0)
{
    memset (&source_node_rid, 0, sizeof (source_node_rid));
}

int zlink::part_helper_internal::validate_send_flags (zlink_send_flags_t flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = ENOTSUP;
        return -1;
    }
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
        || lhs_.request_seq != rhs_.request_seq || lhs_.handler != rhs_.handler
        || lhs_.userdata != rhs_.userdata || lhs_.has_rid1 != rhs_.has_rid1
        || lhs_.has_rid2 != rhs_.has_rid2 || lhs_.has_text1 != rhs_.has_text1
        || lhs_.has_text2 != rhs_.has_text2 || lhs_.request_like != rhs_.request_like) {
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
    return buffer_recv_parts (&state_->recv, parts_, part_count_);
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

int zlink::part_helper_internal::buffer_recv_parts (recv_sequence_state_t *recv_,
                                                    zlink_msg_t *parts_,
                                                    size_t part_count_)
{
    if (!recv_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    recv_->buffered_parts.resize (part_count_);
    recv_->next_part_index = 0;
    for (size_t i = 0; i < part_count_; ++i) {
        zlink_msg_init (&recv_->buffered_parts[i]);
        if (zlink_msg_move (&recv_->buffered_parts[i], &parts_[i]) != 0) {
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

void zlink::part_helper_internal::reset_send_sequence (send_sequence_state_t *state_)
{
    if (!state_)
        return;

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();

    state_->spec = send_sequence_spec_t ();
    state_->send_scope.reset ();
    state_->active = false;
    state_->sink_socket = NULL;
    state_->owner_thread = std::thread::id ();
}

void zlink::part_helper_internal::reset_recv_sequence (recv_sequence_state_t *state_)
{
    if (!state_)
        return;

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();
    state_->next_part_index = 0;

    state_->active = false;
    state_->family = recv_family_none;
    state_->source_socket = NULL;
    state_->owner_thread = std::thread::id ();
    state_->message_type = 0;
}

bool zlink::part_helper_internal::aggregate_send_mode_active ()
{
    return aggregate_send_mode_tls ();
}

void zlink::part_helper_internal::set_aggregate_send_mode (bool active_)
{
    aggregate_send_mode_tls () = active_;
}

int zlink::part_helper_internal::prepare_send_step (void *handle_,
                                                    const send_sequence_spec_t &spec_,
                                                    zlink::socket_base_t *sink_socket_,
                                                    std::shared_ptr<handle_state_t> *state_out_,
                                                    bool *first_part_out_)
{
    if (!state_out_ || !first_part_out_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<handle_state_t> state = find_or_create_handle_state (handle_);
    if (!state)
        return -1;

    std::unique_lock<std::mutex> lock (state->mutex);
    const std::thread::id current_thread = std::this_thread::get_id ();
    while (state->send.active && state->send.owner_thread != current_thread) {
        if (!aggregate_send_mode_active ()) {
            if (routed_part_debug_enabled ()) {
                std::fprintf (stderr,
                              "[routed-part-debug] prepare_send_step busy "
                              "family=%d active_family=%d same_thread=0\n",
                              static_cast<int> (spec_.family),
                              static_cast<int> (state->send.spec.family));
            }
            errno = EINVAL;
            return -1;
        }
        state->cv.wait (lock);
    }

    if (!state->send.active) {
        std::unique_ptr<zlink::socket_public_send_scope_t> send_scope =
          create_send_scope (sink_socket_, spec_);
        if (!send_scope)
            return -1;

        state->send.active = true;
        state->send.spec = spec_;
        state->send.sink_socket = sink_socket_;
        state->send.send_scope = std::move (send_scope);
        state->send.owner_thread = current_thread;
        *first_part_out_ = true;
    } else {
        if (!send_spec_equals (state->send.spec, spec_)) {
            send_sequence_spec_t upgraded = state->send.spec;
            upgraded.timeout_ms = spec_.timeout_ms;
            upgraded.request_seq = spec_.request_seq;
            upgraded.handler = spec_.handler;
            upgraded.userdata = spec_.userdata;
            const bool can_upgrade_staged_request =
              state->send.spec.request_like && spec_.request_like
              && state->send.spec.request_seq == 0 && spec_.request_seq != 0
              && send_spec_equals (upgraded, spec_);
            if (can_upgrade_staged_request) {
                state->send.spec = spec_;
                *first_part_out_ = false;
                *state_out_ = state;
                return 0;
            }
            if (routed_part_debug_enabled ()) {
                std::fprintf (stderr,
                              "[routed-part-debug] prepare_send_step spec "
                              "mismatch family=%d active_family=%d\n",
                              static_cast<int> (spec_.family),
                              static_cast<int> (state->send.spec.family));
            }
            errno = EINVAL;
            return -1;
        }
        *first_part_out_ = false;
    }

    *state_out_ = state;
    return 0;
}

int zlink::part_helper_internal::prepare_staged_send_step (
  void *handle_,
  const send_sequence_spec_t &spec_,
  std::shared_ptr<handle_state_t> *state_out_,
  bool *first_part_out_)
{
    if (!state_out_ || !first_part_out_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<handle_state_t> state = find_or_create_handle_state (handle_);
    if (!state)
        return -1;

    std::unique_lock<std::mutex> lock (state->mutex);
    const std::thread::id current_thread = std::this_thread::get_id ();
    while (state->send.active && state->send.owner_thread != current_thread) {
        if (!aggregate_send_mode_active ()) {
            errno = EINVAL;
            return -1;
        }
        state->cv.wait (lock);
    }

    if (!state->send.active) {
        state->send.active = true;
        state->send.spec = spec_;
        state->send.sink_socket = NULL;
        state->send.owner_thread = current_thread;
        *first_part_out_ = true;
    } else {
        if (!send_spec_equals (state->send.spec, spec_)) {
            send_sequence_spec_t upgraded = state->send.spec;
            upgraded.timeout_ms = spec_.timeout_ms;
            upgraded.handler = spec_.handler;
            upgraded.userdata = spec_.userdata;
            const bool can_upgrade_staged_request =
              state->send.spec.request_like && spec_.request_like
              && state->send.spec.handler == NULL && spec_.handler != NULL
              && send_spec_equals (upgraded, spec_);
            if (!can_upgrade_staged_request) {
                errno = EINVAL;
                return -1;
            }
            state->send.spec = spec_;
        }
        *first_part_out_ = false;
    }

    *state_out_ = state;
    return 0;
}

int zlink::part_helper_internal::stage_staged_send_part (handle_state_t *state_,
                                                         zlink_msg_t *part_)
{
    if (!state_ || !part_) {
        errno = EFAULT;
        return -1;
    }

    state_->send.buffered_parts.resize (state_->send.buffered_parts.size () + 1);
    zlink_msg_t &slot = state_->send.buffered_parts.back ();
    zlink_msg_init (&slot);
    if (zlink_msg_move (&slot, part_) != 0) {
        zlink_msg_close (&slot);
        state_->send.buffered_parts.pop_back ();
        errno = EFAULT;
        return -1;
    }

    return 0;
}

int zlink::part_helper_internal::move_staged_parts_for_submit (
  const std::shared_ptr<handle_state_t> &state_, zlink_msg_t *part_, std::vector<zlink_msg_t> *parts_out_)
{
    if (!state_ || !part_ || !parts_out_) {
        errno = EFAULT;
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        parts_out_->swap (state_->send.buffered_parts);
    }

    parts_out_->resize (parts_out_->size () + 1);
    zlink_msg_t &slot = parts_out_->back ();
    zlink_msg_init (&slot);
    if (zlink_msg_move (&slot, part_) != 0) {
        zlink_msg_close (&slot);
        parts_out_->pop_back ();
        errno = EFAULT;
        return -1;
    }

    return 0;
}

int zlink::part_helper_internal::prepare_recv_step (
  void *handle_,
  recv_family_t family_,
  zlink::socket_base_t *source_socket_,
  std::shared_ptr<handle_state_t> *state_out_,
  bool *first_part_out_,
  zlink::socket_base_t **active_source_socket_out_)
{
    if (!state_out_ || !first_part_out_ || !active_source_socket_out_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<handle_state_t> state = find_or_create_handle_state (handle_);
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
        state->recv.topic_id.clear ();
        memset (&state->recv.source_node_rid, 0, sizeof (state->recv.source_node_rid));
        *first_part_out_ = true;
    } else {
        if (state->recv.family != family_ || state->recv.owner_thread != current_thread) {
            errno = EINVAL;
            return -1;
        }
        *first_part_out_ = false;
    }

    *active_source_socket_out_ = state->recv.source_socket;
    *state_out_ = state;
    return 0;
}

void zlink::part_helper_internal::complete_send_step (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink_part_flag_t part_flag_)
{
    if (!state_ || part_flag_ != ZLINK_PART_FINAL)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    reset_send_sequence (&state_->send);
    state_->cv.notify_all ();
}

void zlink::part_helper_internal::complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink_part_flag_t has_more_)
{
    if (!state_ || has_more_ != ZLINK_PART_FINAL)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    for (size_t i = 0; i < state_->recv.buffered_parts.size (); ++i)
        zlink_msg_close (&state_->recv.buffered_parts[i]);
    state_->recv.buffered_parts.clear ();
    state_->recv.next_part_index = 0;
    state_->recv.active = false;
    state_->recv.family = recv_family_none;
    state_->recv.source_socket = NULL;
    state_->recv.owner_thread = std::thread::id ();
}

void zlink::part_helper_internal::abort_send_step (const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->send.sink_socket && state_->send.send_scope)
        (void) state_->send.sink_socket->rollback_scoped (*state_->send.send_scope);
    reset_send_sequence (&state_->send);
    state_->cv.notify_all ();
}

void zlink::part_helper_internal::abort_recv_step (const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    reset_recv_sequence (&state_->recv);
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

    std::lock_guard<std::mutex> lock (state->mutex);
    reset_recv_sequence (&state->recv);
}
