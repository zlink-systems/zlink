/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <limits>
#include <string>
#include <vector>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_runtime_io_helpers.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/multipart_send_txn.hpp"
#include "core/recv_internal.hpp"
#include "core/recv_tls_view.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
const size_t stack_request_reply_part_capacity = 8;

class reply_target_reservation_t
{
  public:
    explicit reply_target_reservation_t (
      const std::shared_ptr<socket_request_reply_state_t> &state_) :
        _state (state_),
        _active (false)
    {
    }

    bool acquire ()
    {
        if (!_state)
            return false;
        std::lock_guard<std::mutex> lock (_state->mutex);
        if (_state->reply_target_slots >= max_reply_target_slots) {
            errno = EAGAIN;
            return false;
        }
        ++_state->reply_target_slots;
        _active = true;
        return true;
    }

    void commit () { _active = false; }

    ~reply_target_reservation_t ()
    {
        if (_active)
            release_reply_target_slot (_state);
    }

  private:
    const std::shared_ptr<socket_request_reply_state_t> _state;
    bool _active;
};

uint64_t allocate_dealer_reply_token (socket_request_reply_state_t *state_)
{
    if (!state_)
        return 0;

    for (uint64_t i = 0; i < std::numeric_limits<uint64_t>::max (); ++i) {
        uint64_t token = state_->dealer_next_reply_token++;
        if (state_->dealer_next_reply_token == 0)
            state_->dealer_next_reply_token = 1;
        if (token != 0 && state_->dealer_reply_targets.count (token) == 0)
            return token;
    }
    errno = EAGAIN;
    return 0;
}

int validate_request_parts (zlink_msg_t *parts_, size_t part_count_)
{
    if ((!parts_ && part_count_ > 0) || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    return 0;
}

int take_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                              uint64_t request_token_,
                              dealer_reply_target_t *target_out_)
{
    if (!state_ || request_token_ == 0 || !target_out_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    std::unordered_map<uint64_t, dealer_reply_target_t>::iterator it =
      state_->dealer_reply_targets.find (request_token_);
    if (it == state_->dealer_reply_targets.end ()) {
        errno = ENOENT;
        return -1;
    }

    *target_out_ = it->second;
    state_->dealer_reply_targets.erase (it);
    return 0;
}

void restore_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                  uint64_t request_token_,
                                  const dealer_reply_target_t &target_)
{
    if (!state_ || request_token_ == 0)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->dealer_reply_targets[request_token_] = target_;
}

bool take_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t **application_pipe_out_)
{
    if (application_pipe_out_)
        *application_pipe_out_ = NULL;
    if (!state_ || !application_pipe_out_)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    std::unordered_map<pending_key_t, zlink::pipe_t *,
                       pending_key_hash_t>::iterator it =
      state_->router_reply_targets.find (key_);
    if (it == state_->router_reply_targets.end ())
        return false;
    *application_pipe_out_ = it->second;
    state_->router_reply_targets.erase (it);
    return true;
}

void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t *application_pipe_)
{
    if (!state_ || !application_pipe_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->router_reply_targets[key_] = application_pipe_;
}

void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_)
{
    if (!state_ || !application_pipe_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    size_t erased = 0;
    for (std::unordered_map<pending_key_t, zlink::pipe_t *,
                            pending_key_hash_t>::iterator it =
           state_->router_reply_targets.begin ();
         it != state_->router_reply_targets.end ();) {
        if (it->second == application_pipe_) {
            it = state_->router_reply_targets.erase (it);
            ++erased;
        } else
            ++it;
    }
    zlink_assert (state_->reply_target_slots >= erased);
    state_->reply_target_slots -= erased;
}

void release_reply_target_slot (
  const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (state_->reply_target_slots > 0);
    --state_->reply_target_slots;
}

int export_router_payload_parts (zlink_msg_t *parts_,
                                 size_t part_count_,
                                 size_t start_index_,
                                 zlink_msg_t **parts_out_,
                                 size_t *part_count_out_)
{
    if (start_index_ >= part_count_) {
        errno = EPROTO;
        return -1;
    }

    for (size_t i = start_index_; i < part_count_; ++i) {
        if (zlink::recv_tls_view::push (&parts_[i]) != 0) {
            const int saved_errno = errno;
            zlink::recv_tls_view::abort ();
            for (size_t j = i; j < part_count_; ++j)
                zlink_msg_close (&parts_[j]);
            errno = saved_errno;
            return -1;
        }
    }

    return zlink::recv_tls_view::commit (parts_out_, part_count_out_);
}

int recv_router_message_direct (socket_handle_t handle_,
                                const zlink_routing_id_t **source_node_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_)
{
    if (!handle_.socket || !source_node_rid_out_ || !request_seq_out_ || !parts_out_
        || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle_);
    reply_target_reservation_t reply_target_reservation (state);
    if (!reply_target_reservation.acquire ())
        return -1;

    zlink::msg_t current;
    const int current_init_rc = current.init ();
    errno_assert (current_init_rc == 0);
    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    zlink::pipe_t *source_pipe = NULL;
    if (handle_.socket->recv_routed (
          &current, &source_rid, flags_, NULL, &source_pipe)
        != 0) {
        return -1;
    }

    if ((current.flags () & zlink::msg_t::more) == 0) {
        zlink_msg_t *first_slot = NULL;
        if (zlink::recv_tls_view::begin_with_first_slot (parts_out_, part_count_out_, &first_slot)
            != 0) {
            const int saved_errno = errno;
            const int close_rc = current.close ();
            errno_assert (close_rc == 0);
            errno = saved_errno;
            return -1;
        }

        if (reinterpret_cast<zlink::msg_t *> (first_slot)->move (current) != 0) {
            const int saved_errno = errno;
            const int close_rc = current.close ();
            errno_assert (close_rc == 0);
            zlink::recv_tls_view::abort ();
            errno = saved_errno != 0 ? saved_errno : EFAULT;
            return -1;
        }

        router_recv_metadata_tls_t &metadata = router_recv_metadata_tls ();
        assign_routing_id_compact (&metadata.source_rid, source_rid);
        *source_node_rid_out_ = &metadata.source_rid;
        *request_seq_out_ = 0;
        return zlink::recv_tls_view::commit_reserved_single (parts_out_, part_count_out_);
    }

    std::vector<zlink_msg_t> raw_parts;
    raw_parts.reserve (stack_request_reply_part_capacity);
    while (true) {
        raw_parts.push_back (zlink_msg_t ());
        zlink_msg_init (&raw_parts.back ());
        if (reinterpret_cast<zlink::msg_t *> (&raw_parts.back ())->move (current) != 0) {
            zlink::close_msg_frames (&raw_parts);
            const int close_rc = current.close ();
            errno_assert (close_rc == 0);
            errno = EFAULT;
            return -1;
        }

        if (!router_raw_part_has_more (&raw_parts.back ()))
            break;

        zlink_msg_t next;
        zlink_msg_init (&next);
        if (recv_router_followup_frame (handle_.socket, &next) != 0) {
            zlink_msg_close (&next);
            zlink::close_msg_frames (&raw_parts);
            return -1;
        }
        if (current.move (*reinterpret_cast<zlink::msg_t *> (&next)) != 0) {
            zlink_msg_close (&next);
            zlink::close_msg_frames (&raw_parts);
            errno = EFAULT;
            return -1;
        }
    }

    if (zlink::recv_tls_view::begin (parts_out_, part_count_out_) != 0) {
        zlink::close_msg_frames (&raw_parts);
        return -1;
    }

    zlink::request_reply::parsed_envelope_t envelope;
    const bool parsed =
      zlink::request_reply::parse_envelope (raw_parts.data (), raw_parts.size (), &envelope);
    const size_t start_index = parsed ? zlink::request_reply::control_part_count : 0;

    if (parsed) {
        *request_seq_out_ = envelope.request_seq;
        if (envelope.message_type == zlink::request_reply::request_type
            && envelope.request_seq != 0 && source_pipe) {
            if (!state) {
                zlink::close_msg_frames (&raw_parts);
                zlink::recv_tls_view::abort ();
                return -1;
            }
            pending_key_t key;
            key.peer_rid.assign (
              reinterpret_cast<const char *> (source_rid.data),
              source_rid.size);
            key.request_seq = envelope.request_seq;
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                if (!state->router_reply_targets.emplace (key, source_pipe).second) {
                    zlink::close_msg_frames (&raw_parts);
                    zlink::recv_tls_view::abort ();
                    errno = EPROTO;
                    return -1;
                }
            }
            reply_target_reservation.commit ();
        }
        for (size_t i = 0; i < start_index; ++i)
            zlink_msg_close (&raw_parts[i]);
    } else
        *request_seq_out_ = 0;

    router_recv_metadata_tls_t &metadata = router_recv_metadata_tls ();
    metadata.source_rid = source_rid;
    *source_node_rid_out_ = &metadata.source_rid;
    return export_router_payload_parts (raw_parts.data (), raw_parts.size (), start_index,
                                        parts_out_, part_count_out_);
}

int recv_dealer_message_direct (
  socket_handle_t handle_,
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint8_t *message_type_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t **parts_out_,
  size_t *part_count_out_,
  int flags_)
{
    if (!handle_.socket || !state_ || !message_type_out_ || !request_seq_out_
        || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    reply_target_reservation_t reply_target_reservation (state_);
    if (!reply_target_reservation.acquire ())
        return -1;

    zlink::msg_t current;
    const int current_init_rc = current.init ();
    errno_assert (current_init_rc == 0);
    zlink::pipe_t *source_pipe = NULL;
    if (handle_.socket->recv_pipe (&current, &source_pipe, flags_) != 0)
        return -1;

    std::vector<zlink_msg_t> raw_parts;
    raw_parts.reserve (stack_request_reply_part_capacity);
    while (true) {
        const bool more = (current.flags () & zlink::msg_t::more) != 0;
        raw_parts.push_back (zlink_msg_t ());
        zlink_msg_init (&raw_parts.back ());
        if (reinterpret_cast<zlink::msg_t *> (&raw_parts.back ())->move (current) != 0) {
            zlink::close_msg_frames (&raw_parts);
            errno = EFAULT;
            return -1;
        }
        if (!more)
            break;
        zlink::msg_t next;
        const int init_rc = next.init ();
        errno_assert (init_rc == 0);
        if (handle_.socket->recv (&next, ZLINK_DONTWAIT) != 0) {
            const int saved_errno = errno;
            const int close_rc = next.close ();
            errno_assert (close_rc == 0);
            zlink::close_msg_frames (&raw_parts);
            errno = saved_errno;
            return -1;
        }
        const int move_rc = current.move (next);
        errno_assert (move_rc == 0);
    }

    zlink::request_reply::parsed_envelope_t envelope;
    const bool parsed =
      zlink::request_reply::parse_envelope (raw_parts.data (), raw_parts.size (), &envelope);
    size_t start_index = 0;
    uint64_t exported_seq = 0;
    uint8_t exported_type = ZLINK_DEALER_MESSAGE_RAW;
    if (parsed) {
        if (envelope.message_type != zlink::request_reply::request_type) {
            zlink::close_msg_frames (&raw_parts);
            errno = EPROTO;
            return -1;
        }
        if (!source_pipe || envelope.request_seq == 0) {
            zlink::close_msg_frames (&raw_parts);
            errno = EPROTO;
            return -1;
        }
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            exported_seq = allocate_dealer_reply_token (state_.get ());
            if (exported_seq == 0) {
                zlink::close_msg_frames (&raw_parts);
                return -1;
            }
            dealer_reply_target_t target;
            target.pipe = source_pipe;
            target.request_seq = envelope.request_seq;
            state_->dealer_reply_targets[exported_seq] = target;
        }
        reply_target_reservation.commit ();
        exported_type = ZLINK_DEALER_MESSAGE_REQUEST;
        start_index = zlink::request_reply::control_part_count;
        for (size_t i = 0; i < start_index; ++i)
            zlink_msg_close (&raw_parts[i]);
    }

    if (zlink::recv_tls_view::begin (parts_out_, part_count_out_) != 0) {
        zlink::close_msg_frames (&raw_parts);
        return -1;
    }
    if (export_router_payload_parts (raw_parts.data (), raw_parts.size (), start_index,
                                     parts_out_, part_count_out_)
        != 0)
        return -1;
    *message_type_out_ = exported_type;
    *request_seq_out_ = exported_seq;
    return 0;
}

int send_request_reply_message (void *socket_handle_,
                                const zlink_routing_id_t *peer_rid_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_,
                                uint8_t message_type_,
                                uint64_t request_seq_)
{
    const bool valid_sequence =
      message_type_ == zlink::request_reply::completion_control_type
        ? request_seq_ == 0
        : request_seq_ != 0;
    if (!socket_handle_ || !parts_ || part_count_ == 0 || !valid_sequence) {
        errno = EINVAL;
        return -1;
    }

    const socket_handle_t handle = as_socket_handle (socket_handle_);
    if (!handle.socket) {
        errno = EFAULT;
        return -1;
    }

    const bool routed = zlink::valid_routing_id (peer_rid_);
    const size_t total_part_count = zlink::request_reply::control_part_count + part_count_;
    zlink_msg_t stack_combined[stack_request_reply_part_capacity];
    std::vector<zlink_msg_t> heap_combined;
    zlink_msg_t *combined =
      total_part_count <= stack_request_reply_part_capacity ? stack_combined : NULL;
    if (!combined) {
        heap_combined.resize (total_part_count);
        combined = &heap_combined[0];
    }
    for (size_t i = 0; i < total_part_count; ++i)
        zlink_msg_init (&combined[i]);

    if (zlink::request_reply::init_envelope_control_parts (combined, message_type_, request_seq_)
        != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        zlink::request_reply::consume_send_frames_from (parts_, 0, part_count_);
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i < part_count_; ++i) {
        if (zlink_msg_move (&combined[zlink::request_reply::control_part_count + i], &parts_[i])
            != 0) {
            const int saved_errno = errno;
            zlink::request_reply::close_built_parts (combined, total_part_count);
            zlink::request_reply::consume_send_frames_from (parts_, i, part_count_);
            errno = saved_errno;
            return -1;
        }
    }

    if (message_type_ != zlink::request_reply::request_type) {
        //  Replies bypass send()/recv(), so this entry has to drain pending
        //  socket commands itself. Without it the completion pipe stays
        //  backpressured forever: the peer's activate-write command sits in
        //  the mailbox and every retry keeps failing with EAGAIN.
        if (handle.socket->process_submit_commands () != 0) {
            const int saved_errno = errno;
            zlink::request_reply::close_built_parts (combined, total_part_count);
            errno = saved_errno;
            return -1;
        }
        std::shared_ptr<socket_request_reply_state_t> state;
        pending_key_t reply_key;
        zlink::pipe_t *application_pipe = NULL;
        bool target_taken = false;
        if (message_type_ != zlink::request_reply::completion_control_type
            && handle.socket->socket_type () == ZLINK_CORE_SOCKET_ROUTER
            && zlink::valid_routing_id (peer_rid_)) {
            state = find_request_reply_state (handle);
            if (state) {
                reply_key.peer_rid = zlink::routing_id_key (peer_rid_);
                reply_key.request_seq = request_seq_;
                target_taken =
                  take_router_reply_target (state, reply_key, &application_pipe);
            }
        }
        const int rc =
          send_completion_frames (handle.socket, application_pipe, peer_rid_,
                                  combined, total_part_count);
        if (rc != 0) {
            if (target_taken)
                restore_router_reply_target (
                  state, reply_key, application_pipe);
            return -1;
        }
        if (target_taken)
            release_reply_target_slot (state);
        errno = 0;
        return 0;
    }

    router_mandatory_scope_t mandatory_scope;
    if (routed && mandatory_scope.arm (handle) != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        errno = saved_errno;
        return -1;
    }

    const int rc =
      routed ? zlink::logical_multipart_send_routed (handle.socket, peer_rid_, combined,
                                                     total_part_count, flags_)
             : zlink::logical_multipart_send (handle.socket, combined, total_part_count, flags_);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}

int send_completion_frames (zlink::socket_base_t *socket_,
                            zlink::pipe_t *application_pipe_,
                            const zlink_routing_id_t *peer_rid_,
                            zlink_msg_t *parts_,
                            size_t part_count_)
{
    if (!socket_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    zlink::pipe_t *completion =
      application_pipe_
        ? socket_->completion_pipe_for_application (application_pipe_)
        : socket_->completion_pipe_for_peer (peer_rid_);
    if (!completion) {
        socket_->arm_send_ready_after_backpressure ();
        zlink::request_reply::consume_send_frames_from (
          parts_, 0, part_count_);
        errno = EAGAIN;
        return -1;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (i + 1 < part_count_)
            msg->set_flags (zlink::msg_t::more);
        else
            msg->reset_flags (zlink::msg_t::more);
        msg->set_transport_connection_id (
          completion->get_transport_connection_id ());
        const bool written =
          i + 1 < part_count_ ? completion->write (msg) : completion->write_and_flush (msg);
        if (!written) {
            const int saved_errno = errno ? errno : EAGAIN;
            completion->rollback ();
            socket_->arm_send_ready_after_backpressure ();
            zlink::request_reply::consume_send_frames_from (
              parts_, i, part_count_);
            errno = saved_errno;
            return -1;
        }
        const int init_rc = zlink_msg_init (&parts_[i]);
        errno_assert (init_rc == 0);
    }
    errno = 0;
    return 0;
}
}
}
