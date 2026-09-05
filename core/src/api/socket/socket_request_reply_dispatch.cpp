/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/request_reply_frame_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/message/request_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/allocator.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
namespace
{
#ifdef ZLINK_BUILD_TESTS
std::atomic<completion_pipe_budget_exhausted_test_hook_fn>
  completion_pipe_budget_exhausted_test_hook (NULL);
std::atomic<void *> completion_pipe_budget_exhausted_test_hook_userdata (NULL);
#endif

enum completion_message_result_t
{
    completion_message_accepted,
    completion_message_protocol_error
};

int move_completion_payload (zlink_msg_t *parts_, size_t start_,
                             size_t count_, zlink_msg_t **owned_out_)
{
    if (!owned_out_ || (!parts_ && count_ != 0)) {
        errno = EFAULT;
        return -1;
    }
    *owned_out_ = NULL;
    if (count_ == 0)
        return 0;

    zlink_msg_t *owned = NULL;
    try {
#ifdef ZLINK_BUILD_TESTS
        test_throw_request_reply_allocation_failpoint (
          request_reply_allocation_payload_export);
#endif
        owned = static_cast<zlink_msg_t *> (
          zlink::alloc (sizeof (zlink_msg_t) * count_));
    } catch (...) {
        owned = NULL;
    }
    if (!owned) {
        errno = ENOMEM;
        return -1;
    }

    size_t initialized = 0;
    for (; initialized != count_; ++initialized) {
        if (zlink_msg_init (&owned[initialized]) != ZLINK_CONFIG_OK)
            break;
    }
    if (initialized != count_) {
        const int saved_errno = errno;
        zlink_multipart_close (owned, initialized);
        zlink::dealloc (owned);
        errno = saved_errno != 0 ? saved_errno : EIO;
        return -1;
    }
    for (size_t i = 0; i != count_; ++i) {
        if (zlink_msg_move (&owned[i], &parts_[start_ + i])
            != ZLINK_CONFIG_OK) {
            const int saved_errno = errno;
            zlink_multipart_close (owned, count_);
            zlink::dealloc (owned);
            errno = saved_errno != 0 ? saved_errno : EIO;
            return -1;
        }
    }
    *owned_out_ = owned;
    return 0;
}

int publish_pull_reply_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_, uint8_t message_type_, zlink_msg_t *parts_,
  size_t part_count_)
{
    zlink_request_result_t result = ZLINK_REQUEST_OK;
    size_t payload_start = 0;
    size_t payload_count = part_count_;

    if (message_type_ == zlink::request_reply::error_reply_type) {
        result = ZLINK_REQUEST_PROTOCOL_ERROR;
        payload_count = 0;
        if (part_count_ != 0) {
            zlink::msg_t *const errno_part =
              reinterpret_cast<zlink::msg_t *> (&parts_[0]);
            if (errno_part->check () && errno_part->size () == 4) {
                const unsigned char *const bytes =
                  static_cast<const unsigned char *> (errno_part->data ());
                const int reply_errno =
                  static_cast<int> (zlink::get_uint32 (bytes));
                if (reply_errno != 0) {
                    result = zlink::request_result_internal::from_errno (
                      reply_errno);
                    payload_start = 1;
                    payload_count = part_count_ - 1;
                }
            }
        }
    }

    zlink_msg_t *owned_payload = NULL;
    if (payload_count != 0
        && move_completion_payload (parts_, payload_start, payload_count,
                                    &owned_payload)
             != 0) {
        result = ZLINK_REQUEST_INTERNAL_ERROR;
        payload_count = 0;
        owned_payload = NULL;
    }

    return publish_pending_request_completion (
      state_, pending_, result, owned_payload, payload_count);
}

completion_message_result_t complete_reply_from_transport (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  zlink::pipe_t *source_pipe_, uint64_t source_connection_id_,
  uint8_t message_type_,
  uint64_t request_seq_,
  zlink_msg_t *parts_,
  size_t part_count_)
{
    if (!parts_ || part_count_ == 0 || request_seq_ == 0
        || (message_type_ != zlink::request_reply::reply_type
            && message_type_ != zlink::request_reply::error_reply_type)) {
        zlink_multipart_close (parts_, part_count_);
        return completion_message_protocol_error;
    }

    // The sequence is now owned by the pending lookup. Callback-visible
    // payload must not retain transport metadata.
    zlink::request_reply::clear_request_reply_metadata (&parts_[0]);
    if (!state_) {
        zlink_multipart_close (parts_, part_count_);
        return completion_message_accepted;
    }

    pending_request_t pending;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (!take_pending_reply_from_transport_locked (
              state_.get (), request_seq_, transport_pair_id_,
              transport_pair_generation_, source_pipe_,
              source_connection_id_, &pending)) {
            zlink_multipart_close (parts_, part_count_);
            return completion_message_accepted;
        }
    }
    pending.correlation.release ();

    (void) publish_pull_reply_completion (
      state_, &pending, message_type_, parts_, part_count_);
    zlink::request_reply::consume_send_frames_from (parts_, 0, part_count_);
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

completion_pipe_drain_result_t process_completion_pipe (
  zlink::socket_base_t *socket_, zlink::pipe_t *pipe_)
{
    if (!socket_ || !pipe_)
        return completion_pipe_drained;

    std::shared_ptr<socket_request_reply_state_t> state = socket_->request_reply_state ();
    request_reply_frame_buffer_t parts;
    size_t completed_records = 0;
    while (true) {
        if (pipe_->get_transport_lane_count () == 1u) {
            const pipe_normalized_head_kind_t head =
              pipe_->probe_normalized_head_kind ();
            if (head == pipe_head_empty)
                return completion_pipe_drained;
            if (head == pipe_head_data || head == pipe_head_request)
                return completion_pipe_public_head;
            if (head == pipe_head_invalid) {
                pipe_->terminate (false);
                return completion_pipe_terminated;
            }
        }
        // Every exit below closes or consumes the current elements. Keep the
        // buffer storage for the lifetime of this drain so steady-state
        // completions do not allocate once the common part capacity is warm.
        parts.clear ();
        bool complete = false;
        bool flow_state_consumed = false;
        bool completion_delivered_directly = false;
        bool allocation_failed = false;
        uint8_t message_type = zlink::zmp_kind_data;
        uint64_t request_sequence = 0;
        uint64_t source_connection_id = 0;
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
                return truncated_message ? completion_pipe_terminated
                                         : completion_pipe_drained;
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
                        return completion_pipe_terminated;
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
                return completion_pipe_terminated;
            }

            if (parts.empty ()) {
                source_connection_id = frame.transport_connection_id ();
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
                    return completion_pipe_terminated;
                }

                if (!more) {
                    if (complete_reply_from_transport (
                          state, pipe_->get_transport_pair_id (),
                          pipe_->get_transport_pair_generation (), pipe_,
                          source_connection_id,
                          message_type, request_sequence,
                          reinterpret_cast<zlink_msg_t *> (&frame), 1)
                        == completion_message_protocol_error) {
                        pipe_->terminate (false);
                        return completion_pipe_terminated;
                    }
                    completion_delivered_directly = true;
                    complete = true;
                    continue;
                }
            } else {
                if (frame.transport_connection_id ()
                    != source_connection_id) {
                    const int close_rc = frame.close ();
                    errno_assert (close_rc == 0);
                    discard_completion_message_tail (pipe_, more);
                    close_request_reply_frame_buffer (&parts);
                    pipe_->terminate (false);
                    return completion_pipe_terminated;
                }
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
                    return completion_pipe_terminated;
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

        if (allocation_failed) {
            close_request_reply_frame_buffer (&parts);
        } else if (!flow_state_consumed && !completion_delivered_directly) {
            if (complete_reply_from_transport (
                  state, pipe_->get_transport_pair_id (),
                  pipe_->get_transport_pair_generation (), pipe_,
                  source_connection_id, message_type,
                  request_sequence, &parts[0],
                  parts.size ())
                == completion_message_protocol_error) {
                pipe_->terminate (false);
                return completion_pipe_terminated;
            }
        }

        //  Budget only at a record boundary. In particular, never yield after
        //  a MORE part: another pipe may run between records, not inside one.
        ++completed_records;
        if (completed_records == completion_pipe_record_budget) {
#ifdef ZLINK_BUILD_TESTS
            const completion_pipe_budget_exhausted_test_hook_fn hook =
              completion_pipe_budget_exhausted_test_hook.load (
                std::memory_order_acquire);
            if (hook)
                hook (socket_, pipe_,
                      completion_pipe_budget_exhausted_test_hook_userdata.load (
                        std::memory_order_acquire));
#endif
            return completion_pipe_budget_exhausted;
        }
    }
}

#ifdef ZLINK_BUILD_TESTS
void test_set_completion_pipe_budget_exhausted_hook (
  completion_pipe_budget_exhausted_test_hook_fn hook_, void *userdata_)
{
    if (!hook_) {
        completion_pipe_budget_exhausted_test_hook.store (
          NULL, std::memory_order_release);
        completion_pipe_budget_exhausted_test_hook_userdata.store (
          NULL, std::memory_order_release);
        return;
    }
    completion_pipe_budget_exhausted_test_hook_userdata.store (
      userdata_, std::memory_order_release);
    completion_pipe_budget_exhausted_test_hook.store (
      hook_, std::memory_order_release);
}
#endif

bool has_pending_request_work (const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    return !state_->pending_requests.empty ();
}

template <typename TakePending>
static void fail_matching_pending_requests (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink_request_result_t result_, TakePending take_pending_)
{
    const int saved_errno = errno;
    while (true) {
        pending_request_t pending;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            found = take_pending_ (state_.get (), &pending);
        }
        if (!found)
            break;
        pending.correlation.release ();
        (void) publish_pending_request_completion (
          state_, &pending, result_, NULL, 0);
    }
    errno = saved_errno;
}

void fail_pending_requests_for_logical_endpoint (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const std::string &logical_endpoint_)
{
    if (!state_ || logical_endpoint_.empty ())
        return;

    fail_matching_pending_requests (
      state_, ZLINK_REQUEST_NOT_FOUND,
      [&logical_endpoint_] (socket_request_reply_state_t *state,
                            pending_request_t *pending) {
          return take_next_socket_pending_request_for_logical_endpoint_locked (
            state, logical_endpoint_, pending);
      });
}

void fail_pending_requests_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const zlink::pipe_t *pipe_)
{
    if (!state_ || !pipe_)
        return;

    fail_matching_pending_requests (
      state_, ZLINK_REQUEST_NOT_FOUND,
      [pipe_] (socket_request_reply_state_t *state,
                pending_request_t *pending) {
          return take_next_socket_pending_request_for_pipe_locked (
            state, pipe_, pending);
      });
}

void fail_pending_requests_for_transport_pair (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_)
{
    if (!state_ || transport_pair_id_ == 0 || transport_pair_generation_ == 0)
        return;

    fail_matching_pending_requests (
      state_, zlink::request_result_internal::from_errno (EHOSTUNREACH),
      [transport_pair_id_, transport_pair_generation_]
      (socket_request_reply_state_t *state, pending_request_t *pending) {
          return take_next_socket_pending_request_for_transport_pair_locked (
            state, transport_pair_id_, transport_pair_generation_, pending);
      });
}

static void discard_request_reply_state_for_close (
  const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->public_router_reply_checkout_token.store (
          0, std::memory_order_release);
        state_->closing = true;
    }
    cancel_socket_pending_timeouts (state_);
    abandon_public_router_reply_sequence (state_, 0);

    while (true) {
        pending_request_t pending;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            found = take_next_socket_pending_request_locked (state_.get (),
                                                             &pending);
        }
        if (!found)
            break;
        pending.correlation.release ();
        // Close discards unresolved and unread pull records; it does not
        // publish a completion after sealing the socket lifecycle.
        release_pending_request_completion (state_, &pending);
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->dealer_reply_targets.clear ();
    clear_router_reply_targets_locked (state_.get ());
    state_->reply_target_slots =
      state_->reply_target_reservations + state_->reply_target_checkouts;
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

    discard_request_reply_state_for_close (state);
    return 0;
}

void cleanup_request_reply_socket (const socket_handle_t &handle_)
{
    if (!handle_.socket)
        return;

    std::shared_ptr<socket_request_reply_state_t> state = handle_.socket->request_reply_state ();
    if (state)
        discard_request_reply_state_for_close (state);
    handle_.socket->clear_request_reply_state ();
}
}
}
