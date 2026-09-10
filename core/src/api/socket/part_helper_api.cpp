/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <new>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/debug_log.hpp"
#include "utils/routing_id.hpp"

namespace
{
const bool routed_part_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTED_PART_DEBUG");
#ifdef ZLINK_BUILD_TESTS
std::atomic<bool> fail_next_send_caller_identity_allocation (false);
#endif

typedef std::shared_ptr<
  const zlink::part_helper_internal::send_caller_identity_t>
  send_caller_identity_ptr_t;

bool current_send_caller_identity (
  const send_caller_identity_ptr_t **identity_out_,
  bool create_)
{
    static thread_local send_caller_identity_ptr_t identity;
    if (!identity && create_) {
        try {
#ifdef ZLINK_BUILD_TESTS
            if (fail_next_send_caller_identity_allocation.exchange (
                  false, std::memory_order_acq_rel))
                throw std::bad_alloc ();
#endif
            identity = std::make_shared<const zlink::part_helper_internal::send_caller_identity_t> ();
        } catch (...) {
            *identity_out_ = NULL;
            errno = ENOMEM;
            return false;
        }
    }
    *identity_out_ = &identity;
    return true;
}

bool is_publish_family (
  zlink::part_helper_internal::send_family_t family_)
{
    return family_ == zlink::part_helper_internal::send_family_publish;
}

zlink::part_helper_internal::send_sequence_state_t *
find_current_caller_sequence_locked (
  zlink::part_helper_internal::handle_state_t *state_,
  const send_caller_identity_ptr_t *identity_)
{
    if (!state_ || !identity_ || !*identity_ || state_->send_sequences.empty ())
        return NULL;
    zlink::part_helper_internal::send_sequence_store_t::iterator it =
      state_->send_sequences.find (*identity_);
    return it == state_->send_sequences.end () ? NULL : it->second.get ();
}

bool erase_current_caller_sequence_locked (
  zlink::part_helper_internal::handle_state_t *state_,
  zlink::part_helper_internal::send_sequence_state_t *sequence_,
  const send_caller_identity_ptr_t *identity_)
{
    if (!state_ || !sequence_ || !identity_ || !*identity_
        || state_->send_sequences.empty ())
        return false;
    zlink::part_helper_internal::send_sequence_store_t::iterator it =
      state_->send_sequences.find (*identity_);
    if (it == state_->send_sequences.end () || it->second.get () != sequence_)
        return false;
    state_->send_sequences.erase (it);
    state_->send_sequence_count.store (state_->send_sequences.size (),
                                       std::memory_order_release);
    return true;
}

void take_expired_caller_sequences_locked (
  zlink::part_helper_internal::handle_state_t *state_,
  zlink::part_helper_internal::send_sequence_store_t *expired_out_)
{
    if (!state_ || !expired_out_)
        return;
    for (zlink::part_helper_internal::send_sequence_store_t::iterator it =
           state_->send_sequences.begin ();
         it != state_->send_sequences.end ();) {
        if (!it->first.expired ()) {
            ++it;
            continue;
        }
        zlink::part_helper_internal::send_sequence_store_t::iterator expired =
          it++;
        expired_out_->insert (state_->send_sequences.extract (expired));
    }
    if (!expired_out_->empty ())
        state_->send_sequence_count.store (state_->send_sequences.size (),
                                           std::memory_order_release);
}

void release_detached_send_sequences (
  zlink::part_helper_internal::send_sequence_store_t *sequences_)
{
    if (!sequences_)
        return;
    for (zlink::part_helper_internal::send_sequence_store_t::iterator it =
           sequences_->begin (); it != sequences_->end (); ++it)
        zlink::part_helper_internal::reset_send_sequence (it->second.get (),
                                                          false);
    sequences_->clear ();
}

void release_detached_send_ownership (
  zlink::part_helper_internal::send_part_buffer_t *parts_,
  std::shared_ptr<void> *context_)
{
    if (parts_) {
        for (size_t i = 0; i < parts_->size (); ++i)
            zlink_msg_close (&(*parts_)[i]);
        parts_->clear ();
    }
    if (context_)
        context_->reset ();
}

void publish_buffered_recv_readiness (
  zlink::part_helper_internal::recv_sequence_state_t *state_)
{
    if (!state_ || !state_->source_socket)
        return;

    state_->source_socket->set_part_helper_recv_ready (
      state_->active
      && state_->next_part_index < state_->buffered_parts.size ());
}

void assign_send_sequence_spec (
  zlink::part_helper_internal::send_sequence_state_t *destination_,
  const zlink::part_helper_internal::send_sequence_spec_t &source_)
{
    zlink::part_helper_internal::send_sequence_spec_t &spec =
      destination_->spec;
    spec.family = source_.family;
    spec.flags = source_.flags;
    spec.timeout_ms = source_.timeout_ms;
    spec.request_seq = source_.request_seq;
    spec.pending_cookie = source_.pending_cookie;
    spec.has_routing_id = source_.has_routing_id;
    if (source_.has_routing_id)
        zlink::part_helper_internal::copy_routing_id (
          &source_.routing_id, &spec.routing_id);
    spec.has_topic = source_.has_topic;
    if (source_.has_topic) {
        destination_->topic_storage.assign (source_.topic.data (),
                                            source_.topic.size ());
        spec.topic = destination_->topic_storage;
    } else {
        destination_->topic_storage.clear ();
        spec.topic = std::string_view ();
    }
    spec.request_like = source_.request_like;
}

bool send_spec_matches_after_request_upgrade (
  const zlink::part_helper_internal::send_sequence_spec_t &active_,
  const zlink::part_helper_internal::send_sequence_spec_t &incoming_)
{
    using zlink::part_helper_internal::routing_id_equals;
    if (active_.family != incoming_.family
        || active_.flags != incoming_.flags
        || active_.has_routing_id != incoming_.has_routing_id
        || active_.has_topic != incoming_.has_topic
        || active_.request_like != incoming_.request_like)
        return false;
    if (active_.has_routing_id
        && !routing_id_equals (active_.routing_id, incoming_.routing_id))
        return false;
    if (active_.has_topic && active_.topic != incoming_.topic)
        return false;
    return true;
}

void clear_send_sequence_spec (
  zlink::part_helper_internal::send_sequence_state_t *state_)
{
    state_->spec = zlink::part_helper_internal::send_sequence_spec_t ();
    state_->topic_storage.clear ();
}

int begin_send_sequence_locked (
  zlink::part_helper_internal::send_sequence_state_t *sequence_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_,
  zlink::socket_base_t *sink_socket_, const std::thread::id &owner_thread_)
{
    std::optional<zlink::socket_public_send_scope_t> send_scope;
    if (is_publish_family (spec_.family)) {
        // PUB/XPUB still write physical multipart frames incrementally, so
        // they retain the socket-wide multipart marker and control boundary.
        sink_socket_->set_part_helper_send_active (true);
        if (!sink_socket_->begin_public_send_scope (&send_scope)) {
            const int scope_errno = errno;
            sink_socket_->set_part_helper_send_active (false);
            if (scope_errno == EINVAL
                && spec_.flags == ZLINK_SEND_FLAGS_DONTWAIT) {
                sink_socket_->arm_send_recovery_after_backpressure ();
                errno = EAGAIN;
            } else {
                errno = scope_errno;
            }
            return -1;
        }
    }

    try {
        assign_send_sequence_spec (sequence_, spec_);
        sequence_->sink_socket = sink_socket_;
        if (send_scope)
            sequence_->send_scope.emplace (std::move (*send_scope));
        sequence_->owner_thread = owner_thread_;
        sequence_->active = true;
    } catch (const std::bad_alloc &) {
        send_scope.reset ();
        if (is_publish_family (spec_.family))
            sink_socket_->set_part_helper_send_active (false);
        throw;
    }
    return 0;
}

int resume_send_sequence_locked (
  zlink::part_helper_internal::send_sequence_state_t *sequence_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_)
{
    bool commit_upgraded_spec = false;
    if (!zlink::part_helper_internal::send_spec_equals (
          sequence_->spec, spec_)) {
        const bool can_upgrade_staged_request =
          sequence_->spec.request_like && spec_.request_like
          && sequence_->spec.request_seq == 0 && spec_.request_seq != 0
          && send_spec_matches_after_request_upgrade (
            sequence_->spec, spec_);
        if (can_upgrade_staged_request) {
            commit_upgraded_spec = true;
        } else {
            if (zlink::part_helper_internal::routed_part_debug_enabled ()) {
                std::fprintf (
                  stderr,
                  "[routed-part-debug] prepare_send_step spec "
                  "mismatch family=%d active_family=%d\n",
                  static_cast<int> (spec_.family),
                  static_cast<int> (sequence_->spec.family));
            }
            errno = EINVAL;
            return -1;
        }
    }
    if (is_publish_family (spec_.family)) {
        if (!sequence_->send_scope
            || !sequence_->send_scope->resume_multipart_call ()) {
            if (!sequence_->send_scope)
                errno = EFAULT;
            return -1;
        }
    }
    if (commit_upgraded_spec) {
        try {
            assign_send_sequence_spec (sequence_, spec_);
        } catch (...) {
            if (sequence_->send_scope)
                sequence_->send_scope->suspend_multipart_call ();
            throw;
        }
    }
    return 0;
}

int prepare_send_step_state_locked (
  zlink::part_helper_internal::handle_state_t *state_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_,
  zlink::socket_base_t *sink_socket_,
  zlink::part_helper_internal::send_sequence_state_t **sequence_out_,
  bool *first_part_out_,
  bool start_if_inactive_,
  const send_caller_identity_ptr_t *identity_)
{
    const std::thread::id current_thread = std::this_thread::get_id ();
    zlink::part_helper_internal::send_sequence_state_t *sequence = NULL;
    if (is_publish_family (spec_.family))
        sequence = &state_->send;
    else
        sequence = find_current_caller_sequence_locked (state_, identity_);

    if (is_publish_family (spec_.family) && sequence->active
        && sequence->owner_thread != current_thread) {
        if (zlink::part_helper_internal::routed_part_debug_enabled ()) {
            std::fprintf (stderr,
                          "[routed-part-debug] prepare_send_step busy "
                          "family=%d active_family=%d same_thread=0\n",
                          static_cast<int> (spec_.family),
                          static_cast<int> (sequence->spec.family));
        }
        errno = EINVAL;
        return -1;
    }

    try {
        if (!sequence || !sequence->active) {
            if (!start_if_inactive_)
                return 1;
            if (!sequence) {
                std::unique_ptr<
                  zlink::part_helper_internal::send_sequence_state_t> created (
                  new zlink::part_helper_internal::send_sequence_state_t ());
                const std::pair<
                  zlink::part_helper_internal::send_sequence_store_t::iterator,
                  bool> inserted = state_->send_sequences.emplace (
                  *identity_, std::move (created));
                sequence = inserted.first->second.get ();
                if (inserted.second)
                    state_->send_sequence_count.store (
                      state_->send_sequences.size (),
                      std::memory_order_release);
            }
            if (begin_send_sequence_locked (
                  sequence, spec_, sink_socket_, current_thread)
                != 0)
                return -1;
            *first_part_out_ = true;
        } else {
            if (resume_send_sequence_locked (sequence, spec_) != 0)
                return -1;
            *first_part_out_ = false;
        }
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    }
    *sequence_out_ = sequence;
    return 0;
}
}

zlink::part_helper_internal::send_sequence_spec_t::send_sequence_spec_t () :
    family (send_family_none),
    flags (ZLINK_SEND_FLAGS_NONE),
    timeout_ms (0),
    request_seq (0),
    pending_cookie (0),
    has_routing_id (false),
    has_topic (false),
    request_like (false)
{
    memset (&routing_id, 0, sizeof (routing_id));
}

zlink::part_helper_internal::send_sequence_state_t::send_sequence_state_t () :
    active (false),
    sink_socket (NULL)
{
}

zlink::part_helper_internal::send_sequence_state_t::~send_sequence_state_t ()
{
    reset_send_sequence (this, false);
}

#ifdef ZLINK_BUILD_TESTS
void zlink::part_helper_internal::test_fail_next_send_caller_identity_allocation ()
{
    fail_next_send_caller_identity_allocation.store (true,
                                                      std::memory_order_release);
}
#endif

zlink::part_helper_internal::recv_sequence_state_t::recv_sequence_state_t () :
    active (false),
    family (recv_family_none),
    source_socket (NULL),
    owner_thread (),
    return_source_rid_as_null (true),
    request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
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

int zlink::part_helper_internal::validate_whole_send (
  zlink::socket_base_t *socket_, zlink_msg_t *parts_, size_t part_count_,
  int argument_errno_)
{
    if (!parts_ || argument_errno_ != 0) {
        errno = !parts_ ? EFAULT : argument_errno_;
        return -1;
    }
    if (part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_)
        return -1;
    // Whole records cannot complete a prefix staged through the additive
    // part API. Other threads' logical send slots remain independent.
    if (current_send_sequence_active (socket_)
        || socket_->part_helper_send_active ()) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

bool zlink::part_helper_internal::try_rollback_send_scope_locked (
  send_sequence_state_t *state_)
{
    if (!state_ || !state_->sink_socket || !state_->send_scope)
        return false;
    if (!state_->send_scope->acquired ()
        && !state_->send_scope->resume_multipart_call ()
        && !state_->send_scope->lock_multipart_for_close_cleanup ())
        return false;

    (void) state_->sink_socket->rollback_scoped (*state_->send_scope);
    return true;
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

bool zlink::part_helper_internal::send_spec_equals (const send_sequence_spec_t &lhs_,
                                                    const send_sequence_spec_t &rhs_)
{
    if (lhs_.family != rhs_.family || lhs_.flags != rhs_.flags || lhs_.timeout_ms != rhs_.timeout_ms
        || lhs_.request_seq != rhs_.request_seq
        || lhs_.pending_cookie != rhs_.pending_cookie
        || lhs_.has_routing_id != rhs_.has_routing_id
        || lhs_.has_topic != rhs_.has_topic
        || lhs_.request_like != rhs_.request_like) {
        return false;
    }

    if (lhs_.has_routing_id
        && !routing_id_equals (lhs_.routing_id, rhs_.routing_id))
        return false;
    if (lhs_.has_topic && lhs_.topic != rhs_.topic)
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

zlink::part_helper_internal::staged_recv_record_result_t
zlink::part_helper_internal::try_take_staged_recv_record (
  const std::shared_ptr<handle_state_t> &state_, recv_family_t family_,
  zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
  recv_record_metadata_t *metadata_out_)
{
    if (!state_)
        return staged_recv_record_none;
    if (!parts_out_ || !part_count_out_ || !metadata_out_) {
        errno = EFAULT;
        return staged_recv_record_error;
    }

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        recv_sequence_state_t &recv = state_->recv;
        if (!recv.active)
            return staged_recv_record_none;
        if (recv.family != family_
            || recv.owner_thread != std::this_thread::get_id ()
            || recv.next_part_index != 0) {
            errno = EBUSY;
            return staged_recv_record_error;
        }

        const size_t part_count = recv.buffered_parts.size ();
        if (parts_capacity_ < part_count) {
            *part_count_out_ = part_count;
            errno = ENOBUFS;
            return staged_recv_record_error;
        }

        metadata_out_->return_source_rid_as_null =
          recv.return_source_rid_as_null;
        metadata_out_->source_node_rid = recv.source_node_rid;
        metadata_out_->request_seq = recv.request_seq;
        metadata_out_->transport_pair_id = recv.transport_pair_id;
        metadata_out_->transport_pair_generation =
          recv.transport_pair_generation;
        // Caller slots are uninitialized (whole-message recv does not require
        // init), so adopt rather than move: adopt overwrites without inspecting
        // or closing the destination, avoiding an uninitialized read.
        for (size_t i = 0; i < part_count; ++i) {
            const int adopt_rc =
              zlink_msg_adopt (&parts_out_[i], &recv.buffered_parts[i]);
            errno_assert (adopt_rc == 0);
        }
        *part_count_out_ = part_count;
        held_socket = reset_recv_sequence (&recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
    errno = 0;
    return staged_recv_record_taken;
}

int zlink::part_helper_internal::stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                                                      recv_family_t family_,
                                                      zlink::socket_base_t *source_socket_,
                                                      const zlink_routing_id_t *source_node_rid_,
                                                      uint64_t request_seq_,
                                                      zlink_msg_t *parts_,
                                                      size_t part_count_,
                                                      std::thread::id owner_thread_,
                                                      uint64_t transport_pair_id_,
                                                      uint64_t transport_pair_generation_)
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
    state_->recv.transport_pair_id = transport_pair_id_;
    state_->recv.transport_pair_generation =
      transport_pair_generation_;
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

    publish_buffered_recv_readiness (recv_);
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
    publish_buffered_recv_readiness (recv_);
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

    clear_send_sequence_spec (state_);
    state_->send_scope.reset ();
    state_->family_context.reset ();
    state_->active = false;
    state_->sink_socket = NULL;
    state_->owner_thread = std::thread::id ();

    if (sink_socket && had_send_scope)
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

    state_->active = false;
    publish_buffered_recv_readiness (state_);

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();
    state_->next_part_index = 0;

    state_->family = recv_family_none;
    state_->source_socket = NULL;
    state_->owner_thread = std::thread::id ();
    state_->return_source_rid_as_null = true;
    copy_routing_id (NULL, &state_->source_node_rid);
    state_->request_seq = 0;
    state_->transport_pair_id = 0;
    state_->transport_pair_generation = 0;
    state_->subscribed = 0;
    state_->topic_id.clear ();
    state_->public_delivery_hold = false;
    return held_socket;
}

int zlink::part_helper_internal::prepare_send_step (const send_sequence_spec_t &spec_,
                                                    zlink::socket_base_t *sink_socket_,
                                                    std::shared_ptr<handle_state_t> *state_out_,
                                                    send_sequence_state_t **sequence_out_,
                                                    bool *first_part_out_)
{
    if (!state_out_ || !sequence_out_ || !first_part_out_) {
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

    const send_caller_identity_ptr_t *identity = NULL;
    if (!is_publish_family (spec_.family)
        && !current_send_caller_identity (&identity, true))
        return -1;
    std::lock_guard<std::mutex> lock (state->mutex);
    if (prepare_send_step_state_locked (state.get (), spec_, sink_socket_, sequence_out_,
                                        first_part_out_, true, identity)
        != 0)
        return -1;

    *state_out_ = state;
    return 0;
}

int zlink::part_helper_internal::prepare_send_step_locked (
  const send_sequence_spec_t &spec_, zlink::socket_base_t *sink_socket_,
  handle_state_t **state_out_,
  send_sequence_state_t **sequence_out_,
  std::unique_lock<std::mutex> *lock_out_, bool *first_part_out_,
  bool start_if_inactive_)
{
    if (!state_out_ || !sequence_out_ || !lock_out_ || !first_part_out_
        || !sink_socket_) {
        errno = EFAULT;
        return -1;
    }

    handle_state_t *state = sink_socket_->borrow_part_helper_state ();
    std::shared_ptr<handle_state_t> created_owner;
    if (!state && start_if_inactive_) {
        // Creation and competing installers retain the existing shared-owner
        // path. Once installed, the socket owns that state until destruction.
        created_owner = find_or_create_socket_state (sink_socket_);
        state = created_owner.get ();
    }
    if (!state)
        return start_if_inactive_ ? -1 : 1;

    if (!is_publish_family (spec_.family) && !start_if_inactive_
        // This thread published its own slot under the helper mutex, so a
        // relaxed zero proves that it has no slot without consulting TLS.
        && state->send_sequence_count.load (std::memory_order_relaxed) == 0)
        return 1;

    const send_caller_identity_ptr_t *identity = NULL;
    if (!is_publish_family (spec_.family)
        && !current_send_caller_identity (&identity, start_if_inactive_))
        return -1;
    if (!is_publish_family (spec_.family) && (!identity || !*identity))
        return 1;

    send_sequence_store_t expired;
    std::unique_lock<std::mutex> lock (state->mutex);
    take_expired_caller_sequences_locked (state, &expired);
    if (!expired.empty ()) {
        lock.unlock ();
        release_detached_send_sequences (&expired);
        lock.lock ();
    }
    const int rc = prepare_send_step_state_locked (
      state, spec_, sink_socket_, sequence_out_, first_part_out_,
      start_if_inactive_, identity);
    if (rc != 0)
        return rc;

    *state_out_ = state;
    *lock_out_ = std::move (lock);
    return 0;
}

zlink::part_helper_internal::send_sequence_state_t *
zlink::part_helper_internal::find_current_send_sequence_locked (
  handle_state_t *state_)
{
    const send_caller_identity_ptr_t *identity = NULL;
    if (!current_send_caller_identity (&identity, false))
        return NULL;
    return find_current_caller_sequence_locked (state_, identity);
}

zlink::part_helper_internal::handle_state_t *
zlink::part_helper_internal::borrow_send_sequence_state (
  zlink::socket_base_t *socket_)
{
    handle_state_t *const state =
      socket_ ? socket_->borrow_part_helper_state () : NULL;
    return state
               && state->send_sequence_count.load (std::memory_order_relaxed)
                    != 0
             ? state
             : NULL;
}

bool zlink::part_helper_internal::current_send_sequence_active (
  zlink::socket_base_t *socket_)
{
    handle_state_t *const state = borrow_send_sequence_state (socket_);
    if (!state)
        return false;
    const send_caller_identity_ptr_t *identity = NULL;
    if (!current_send_caller_identity (&identity, false) || !identity
        || !*identity)
        return false;
    std::lock_guard<std::mutex> lock (state->mutex);
    send_sequence_state_t *const sequence =
      find_current_caller_sequence_locked (state, identity);
    return sequence && sequence->active;
}

int zlink::part_helper_internal::prepare_recv_step (
  recv_family_t family_,
  zlink::socket_base_t *source_socket_,
  const std::shared_ptr<handle_state_t> &state_,
  bool *first_part_out_,
  zlink::socket_base_t **active_source_socket_out_)
{
    if (!state_ || !first_part_out_ || !active_source_socket_out_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    const std::thread::id current_thread = std::this_thread::get_id ();

    if (!state_->recv.active) {
        state_->recv.active = true;
        state_->recv.family = family_;
        state_->recv.source_socket = source_socket_;
        state_->recv.owner_thread = current_thread;
        state_->recv.return_source_rid_as_null = true;
        state_->recv.request_seq = 0;
        state_->recv.transport_pair_id = 0;
        state_->recv.transport_pair_generation = 0;
        state_->recv.topic_id.clear ();
        memset (&state_->recv.source_node_rid, 0,
                sizeof (state_->recv.source_node_rid));
        *first_part_out_ = true;
    } else {
        if (state_->recv.family != family_
            || state_->recv.owner_thread != current_thread) {
            errno = EBUSY;
            return -1;
        }
        *first_part_out_ = false;
    }

    *active_source_socket_out_ = state_->recv.source_socket;
    return 0;
}

void zlink::part_helper_internal::complete_send_step (const std::shared_ptr<handle_state_t> &state_,
                                                      send_sequence_state_t *sequence_,
                                                      zlink_part_flag_t part_flag_)
{
    complete_send_step (state_.get (), sequence_, part_flag_);
}

void zlink::part_helper_internal::complete_send_step (
  handle_state_t *state_, send_sequence_state_t *sequence_,
  zlink_part_flag_t part_flag_)
{
    if (!state_ || !sequence_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    complete_send_step_locked (state_, sequence_, part_flag_);
}

void zlink::part_helper_internal::complete_send_step_locked (
  handle_state_t *state_, send_sequence_state_t *sequence_,
  zlink_part_flag_t part_flag_)
{
    if (!state_ || !sequence_)
        return;

    if (part_flag_ == ZLINK_PART_MORE) {
        if (sequence_ == &state_->send) {
            if (sequence_->sink_socket)
                sequence_->sink_socket->hold_incremental_send_control_boundary ();
            if (sequence_->send_scope)
                sequence_->send_scope->suspend_multipart_call ();
        }
        return;
    }
    if (part_flag_ != ZLINK_PART_FINAL)
        return;

    if (sequence_ == &state_->send)
        reset_send_sequence (sequence_);
    else {
        const send_caller_identity_ptr_t *identity = NULL;
        if (current_send_caller_identity (&identity, false))
            (void) erase_current_caller_sequence_locked (state_, sequence_,
                                                          identity);
    }
}

int zlink::part_helper_internal::take_buffered_send_record_locked (
  handle_state_t *state_, send_sequence_state_t *sequence_,
  send_part_buffer_t *parts_out_)
{
    if (!state_ || !sequence_ || !parts_out_ || !parts_out_->empty ()) {
        errno = EFAULT;
        return -1;
    }
    if (!sequence_->active || sequence_->buffered_parts.empty ()
        || (sequence_ == &state_->send
            && sequence_->owner_thread != std::this_thread::get_id ())) {
        errno = EINVAL;
        return -1;
    }

    parts_out_->take_from (&sequence_->buffered_parts);
    if (sequence_ == &state_->send)
        reset_send_sequence (sequence_);
    else {
        const send_caller_identity_ptr_t *identity = NULL;
        if (!current_send_caller_identity (&identity, false)
            || !erase_current_caller_sequence_locked (state_, sequence_,
                                                        identity)) {
            errno = EINVAL;
            return -1;
        }
    }
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

void zlink::part_helper_internal::abort_send_step (
  const std::shared_ptr<handle_state_t> &state_,
  send_sequence_state_t *sequence_)
{
    abort_send_step (state_.get (), sequence_);
}

void zlink::part_helper_internal::abort_send_step (
  handle_state_t *state_, send_sequence_state_t *sequence_)
{
    if (!state_ || !sequence_)
        return;

    send_part_buffer_t discarded;
    std::shared_ptr<void> discarded_context;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (sequence_ == &state_->send) {
            if (sequence_->sink_socket && sequence_->send_scope
                && !try_rollback_send_scope_locked (sequence_))
                return;
            reset_send_sequence (sequence_);
            return;
        }
        discarded.take_from (&sequence_->buffered_parts);
        discarded_context = std::move (sequence_->family_context);
        const send_caller_identity_ptr_t *identity = NULL;
        if (current_send_caller_identity (&identity, false))
            (void) erase_current_caller_sequence_locked (state_, sequence_,
                                                          identity);
    }
    release_detached_send_ownership (&discarded, &discarded_context);
}

void zlink::part_helper_internal::abort_current_non_publish_send_sequence (void *handle_)
{
    const int saved_errno = errno;
    // Keep the public handle pin until both payload callbacks and a detached
    // family context (including REPLY token restore) have completed.
    socket_handle_t handle = as_socket_handle (handle_);
    std::shared_ptr<handle_state_t> state;
    if (handle.socket)
        state = find_socket_state (handle.socket);
    if (state) {
        if (state->send_sequence_count.load (std::memory_order_relaxed) == 0) {
            errno = saved_errno;
            return;
        }
        const send_caller_identity_ptr_t *identity = NULL;
        if (!current_send_caller_identity (&identity, false) || !identity
            || !*identity) {
            errno = saved_errno;
            return;
        }
        send_part_buffer_t discarded;
        std::shared_ptr<void> discarded_context;
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            send_sequence_state_t *const sequence =
              find_current_caller_sequence_locked (state.get (), identity);
            if (sequence) {
                discarded.take_from (&sequence->buffered_parts);
                discarded_context = std::move (sequence->family_context);
                (void) erase_current_caller_sequence_locked (state.get (),
                                                              sequence,
                                                              identity);
            }
        }
        release_detached_send_ownership (&discarded, &discarded_context);
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
