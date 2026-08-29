/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"


#include <limits>
#include <string>
#include <vector>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/request_reply_frame_buffer_internal.hpp"
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
        if (!_state) {
            errno = EFAULT;
            return false;
        }
        std::lock_guard<std::mutex> lock (_state->mutex);
        if (_state->closing) {
            errno = ETERM;
            return false;
        }
        if (_state->reply_target_slots >= max_reply_target_slots) {
            errno = EAGAIN;
            return false;
        }
        ++_state->reply_target_slots;
        ++_state->reply_target_reservations;
        _active = true;
        return true;
    }

    bool commit_locked ()
    {
        if (!_active)
            return false;
        zlink_assert (_state->reply_target_reservations > 0);
        --_state->reply_target_reservations;
        _active = false;
        if (_state->closing) {
            zlink_assert (_state->reply_target_slots > 0);
            --_state->reply_target_slots;
            errno = ETERM;
            return false;
        }
        return true;
    }

    ~reply_target_reservation_t ()
    {
        if (!_active)
            return;
        std::lock_guard<std::mutex> lock (_state->mutex);
        zlink_assert (_state->reply_target_reservations > 0);
        zlink_assert (_state->reply_target_slots > 0);
        --_state->reply_target_reservations;
        --_state->reply_target_slots;
    }

  private:
    const std::shared_ptr<socket_request_reply_state_t> _state;
    bool _active;
};

class reply_target_publish_guard_t
{
  public:
    reply_target_publish_guard_t () :
        _router_key (NULL),
        _dealer_token (0),
        _active (false)
    {
    }

    ~reply_target_publish_guard_t ()
    {
        if (!_active || !_state)
            return;

        std::lock_guard<std::mutex> lock (_state->mutex);
        bool erased = false;
        if (_router_key) {
            std::unordered_map<pending_key_t, router_reply_target_t,
                               pending_key_hash_t>::iterator it =
              _state->router_reply_targets.find (*_router_key);
            if (it != _state->router_reply_targets.end ()) {
                zlink_assert (!it->second.checked_out);
                if (!it->second.checked_out) {
                    _state->router_reply_targets.erase (it);
                    erased = true;
                }
            }
        } else if (_dealer_token != 0) {
            std::unordered_map<uint64_t, dealer_reply_target_t>::iterator it =
              _state->dealer_reply_targets.find (_dealer_token);
            if (it != _state->dealer_reply_targets.end ()) {
                zlink_assert (!it->second.checked_out);
                if (!it->second.checked_out) {
                    _state->dealer_reply_targets.erase (it);
                    erased = true;
                }
            }
        }

        // Close clears published targets and recomputes the slot count while
        // holding the same mutex. A missing node therefore means close already
        // released this slot.
        if (erased) {
            zlink_assert (_state->reply_target_slots > 0);
            --_state->reply_target_slots;
        }
    }

    void arm_router (const std::shared_ptr<socket_request_reply_state_t> &state_,
                     const pending_key_t *key_)
    {
        _state = state_;
        _router_key = key_;
        _dealer_token = 0;
        _active = state_ && key_;
    }

    void arm_dealer (const std::shared_ptr<socket_request_reply_state_t> &state_,
                     uint64_t token_)
    {
        _state = state_;
        _router_key = NULL;
        _dealer_token = token_;
        _active = state_ && token_ != 0;
    }

    void release () { _active = false; }

  private:
    std::shared_ptr<socket_request_reply_state_t> _state;
    const pending_key_t *_router_key;
    uint64_t _dealer_token;
    bool _active;
};

void discard_received_message_tail (zlink::socket_base_t *socket_,
                                    zlink::msg_t *current_,
                                    bool current_has_more_,
                                    bool reinitialize_current_)
{
    if (current_) {
        const int close_rc = current_->close ();
        errno_assert (close_rc == 0);
        if (reinitialize_current_) {
            const int init_rc = current_->init ();
            errno_assert (init_rc == 0);
        }
    }

    bool more = current_has_more_;
    while (more && socket_) {
        zlink::msg_t next;
        const int init_rc = next.init ();
        errno_assert (init_rc == 0);
        if (socket_->recv (&next, ZLINK_DONTWAIT) != 0) {
            const int close_rc = next.close ();
            errno_assert (close_rc == 0);
            return;
        }
        more = (next.flags () & zlink::msg_t::more) != 0;
        const int close_rc = next.close ();
        errno_assert (close_rc == 0);
    }
}

int begin_payload_export (zlink_msg_t **parts_out_,
                          size_t *part_count_out_)
{
    try {
        return zlink::recv_tls_view::begin (parts_out_, part_count_out_);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
}

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
    if (it == state_->dealer_reply_targets.end () || it->second.checked_out) {
        errno = ENOENT;
        return -1;
    }

    *target_out_ = it->second;
    it->second.checked_out = true;
    zlink_assert (state_->reply_target_slots > 0);
    ++state_->reply_target_checkouts;
    return 0;
}

void restore_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                  uint64_t request_token_)
{
    if (!state_ || request_token_ == 0)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (state_->reply_target_checkouts > 0);
    --state_->reply_target_checkouts;
    std::unordered_map<uint64_t, dealer_reply_target_t>::iterator it =
      state_->dealer_reply_targets.find (request_token_);
    if (state_->closing || it == state_->dealer_reply_targets.end ()) {
        zlink_assert (state_->closing);
        zlink_assert (state_->reply_target_slots > 0);
        if (it != state_->dealer_reply_targets.end ())
            state_->dealer_reply_targets.erase (it);
        --state_->reply_target_slots;
        return;
    }
    zlink_assert (it->second.checked_out);
    it->second.checked_out = false;
}

void commit_dealer_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t request_token_)
{
    if (!state_ || request_token_ == 0)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (state_->reply_target_checkouts > 0);
    zlink_assert (state_->reply_target_slots > 0);
    std::unordered_map<uint64_t, dealer_reply_target_t>::iterator it =
      state_->dealer_reply_targets.find (request_token_);
    if (it != state_->dealer_reply_targets.end ()) {
        zlink_assert (it->second.checked_out);
        state_->dealer_reply_targets.erase (it);
    } else
        zlink_assert (state_->closing);
    --state_->reply_target_checkouts;
    --state_->reply_target_slots;
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
    std::unordered_map<pending_key_t, router_reply_target_t,
                       pending_key_hash_t>::iterator it =
      state_->router_reply_targets.find (key_);
    if (it == state_->router_reply_targets.end () || it->second.checked_out
        || !it->second.pipe)
        return false;
    *application_pipe_out_ = it->second.pipe;
    it->second.checked_out = true;
    zlink_assert (state_->reply_target_slots > 0);
    ++state_->reply_target_checkouts;
    return true;
}

void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (state_->reply_target_checkouts > 0);
    --state_->reply_target_checkouts;
    std::unordered_map<pending_key_t, router_reply_target_t,
                       pending_key_hash_t>::iterator it =
      state_->router_reply_targets.find (key_);
    if (state_->closing || it == state_->router_reply_targets.end ()
        || !it->second.pipe) {
        zlink_assert (state_->closing || it != state_->router_reply_targets.end ());
        zlink_assert (state_->reply_target_slots > 0);
        if (it != state_->router_reply_targets.end ())
            state_->router_reply_targets.erase (it);
        --state_->reply_target_slots;
        return;
    }
    zlink_assert (it->second.checked_out);
    it->second.checked_out = false;
}

void commit_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (state_->reply_target_checkouts > 0);
    zlink_assert (state_->reply_target_slots > 0);
    std::unordered_map<pending_key_t, router_reply_target_t,
                       pending_key_hash_t>::iterator it =
      state_->router_reply_targets.find (key_);
    if (it != state_->router_reply_targets.end ()) {
        zlink_assert (it->second.checked_out);
        state_->router_reply_targets.erase (it);
    } else
        zlink_assert (state_->closing);
    --state_->reply_target_checkouts;
    --state_->reply_target_slots;
}

void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_)
{
    if (!state_ || !application_pipe_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    size_t erased = 0;
    for (std::unordered_map<pending_key_t, router_reply_target_t,
                            pending_key_hash_t>::iterator it =
           state_->router_reply_targets.begin ();
         it != state_->router_reply_targets.end ();) {
        if (it->second.pipe == application_pipe_) {
            if (it->second.checked_out) {
                // The in-flight reply still owns this slot. Mark the retained
                // node unusable so restore drops it without allocating or
                // resurrecting a disconnected pipe.
                it->second.pipe = NULL;
                ++it;
            } else {
                it = state_->router_reply_targets.erase (it);
                ++erased;
            }
        } else
            ++it;
    }
    zlink_assert (state_->reply_target_slots >= erased);
    state_->reply_target_slots -= erased;
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
        int push_rc = -1;
        try {
#ifdef ZLINK_BUILD_TESTS
            test_throw_request_reply_allocation_failpoint (
              request_reply_allocation_payload_export);
#endif
            push_rc = zlink::recv_tls_view::push (&parts_[i]);
        } catch (...) {
            errno = ENOMEM;
        }
        if (push_rc != 0) {
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

int recv_router_message_direct (const socket_handle_t &handle_,
                                const zlink_routing_id_t **source_node_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_,
                                bool *terminal_part_returned_out_,
                                zlink_routing_id_t *terminal_source_storage_)
{
    if (!handle_.socket || !source_node_rid_out_ || !request_seq_out_ || !parts_out_
        || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (terminal_part_returned_out_)
        *terminal_part_returned_out_ = false;

    // Raw ROUTER traffic does not own request/reply state. Only consult an
    // already-active request surface here so its bounded reply-target contract
    // can stop us before consuming another message. Pure routed data therefore
    // stays out of the request registry and its mutex entirely.
    std::shared_ptr<socket_request_reply_state_t> state =
      handle_.socket->has_request_reply_state ()
        ? handle_.socket->request_reply_state ()
        : std::shared_ptr<socket_request_reply_state_t> ();
    if (state) {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (state->closing) {
            errno = ETERM;
            return -1;
        }
        if (state->reply_target_slots >= max_reply_target_slots) {
            errno = EAGAIN;
            return -1;
        }
    }

    // Part receive APIs accept an uninitialised output slot. Only use the
    // zero-copy terminal path when that slot is already a valid msg_t;
    // otherwise receive into local storage and export through the normal
    // move path, which initializes the caller's slot.
    const bool receive_terminal_direct =
      terminal_part_out_ && terminal_part_returned_out_
      && reinterpret_cast<zlink::msg_t *> (terminal_part_out_)->check ();
    zlink::msg_t current_storage;
    if (!receive_terminal_direct) {
        const int current_init_rc = current_storage.init ();
        errno_assert (current_init_rc == 0);
    }
    zlink::msg_t &current =
      receive_terminal_direct
        ? *reinterpret_cast<zlink::msg_t *> (terminal_part_out_)
        : current_storage;
    router_recv_metadata_tls_t &metadata = router_recv_metadata_tls ();
    zlink_routing_id_t *const source_rid =
      terminal_source_storage_ ? terminal_source_storage_
                               : &metadata.source_rid;
    memset (source_rid, 0, sizeof (*source_rid));
    zlink::pipe_t *source_pipe = NULL;
    const int first_recv_rc = handle_.socket->recv_routed (
      &current, source_rid, flags_, NULL, &source_pipe);
    if (first_recv_rc != 0)
        return -1;

    metadata.transport_pair_id = source_pipe ? source_pipe->get_transport_pair_id () : 0;
    metadata.transport_pair_generation =
      source_pipe ? source_pipe->get_transport_pair_generation () : 0;

    if ((current.flags () & zlink::msg_t::more) == 0) {
        zlink_routing_id_t *const exported_source = source_rid;
        *source_node_rid_out_ = exported_source;
        *request_seq_out_ = 0;
        if (receive_terminal_direct) {
            // The part API already owns an initialized destination. A terminal
            // raw frame was received there directly; no multipart TLS view or
            // request-envelope state exists for this data-plane role.
            *terminal_part_returned_out_ = true;
            return 0;
        }
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

        const int export_rc = zlink::recv_tls_view::commit_reserved_single (
          parts_out_, part_count_out_);
        return export_rc;
    }

    if (!state) {
        try {
#ifdef ZLINK_BUILD_TESTS
            test_throw_request_reply_allocation_failpoint (
              request_reply_allocation_lazy_state_create);
#endif
            state = find_or_create_request_reply_state (handle_);
        } catch (...) {
            discard_received_message_tail (
              handle_.socket, &current, true, receive_terminal_direct);
            errno = ENOMEM;
            return -1;
        }
        if (!state) {
            const int saved_errno = errno;
            discard_received_message_tail (
              handle_.socket, &current, true, receive_terminal_direct);
            errno = saved_errno;
            return -1;
        }
    }
    reply_target_reservation_t reply_target_reservation (state);
    if (!reply_target_reservation.acquire ()) {
        const int saved_errno = errno;
        discard_received_message_tail (
          handle_.socket, &current, true, receive_terminal_direct);
        errno = saved_errno;
        return -1;
    }

    request_reply_frame_buffer_t raw_parts;
    while (true) {
        const bool more = (current.flags () & zlink::msg_t::more) != 0;
        try {
#ifdef ZLINK_BUILD_TESTS
            if (raw_parts.size () == inline_request_reply_frame_capacity)
                test_throw_request_reply_allocation_failpoint (
                  request_reply_allocation_receive_spill);
#endif
            raw_parts.push_back (zlink_msg_t ());
        } catch (...) {
            zlink::close_msg_frames (&raw_parts);
            discard_received_message_tail (
              handle_.socket, &current, more, receive_terminal_direct);
            errno = ENOMEM;
            return -1;
        }
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
        const int followup_rc = recv_router_followup_frame (
          handle_.socket, &next);
        if (followup_rc != 0) {
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

    if (begin_payload_export (parts_out_, part_count_out_) != 0) {
        zlink::close_msg_frames (&raw_parts);
        return -1;
    }

    zlink::request_reply::parsed_envelope_t envelope;
    const bool parsed =
      zlink::request_reply::parse_envelope (raw_parts.data (), raw_parts.size (), &envelope);
    const size_t start_index = parsed ? zlink::request_reply::control_part_count : 0;

    pending_key_t published_reply_key;
    reply_target_publish_guard_t published_reply_guard;
    if (parsed) {
        *request_seq_out_ = envelope.request_seq;
        if (envelope.message_type == zlink::request_reply::request_type
            && envelope.request_seq != 0 && source_pipe) {
            if (!state) {
                zlink::close_msg_frames (&raw_parts);
                zlink::recv_tls_view::abort ();
                return -1;
            }
            try {
                published_reply_key.peer_rid.assign (
                  reinterpret_cast<const char *> (source_rid->data),
                  source_rid->size);
            } catch (...) {
                zlink::close_msg_frames (&raw_parts);
                zlink::recv_tls_view::abort ();
                errno = ENOMEM;
                return -1;
            }
            published_reply_key.request_seq = envelope.request_seq;
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                if (state->closing) {
                    zlink::close_msg_frames (&raw_parts);
                    zlink::recv_tls_view::abort ();
                    errno = ETERM;
                    return -1;
                }
                router_reply_target_t target;
                target.pipe = source_pipe;
                bool inserted = false;
                try {
                    inserted = state->router_reply_targets.emplace (
                      published_reply_key, target).second;
                } catch (...) {
                    zlink::close_msg_frames (&raw_parts);
                    zlink::recv_tls_view::abort ();
                    errno = ENOMEM;
                    return -1;
                }
                if (!inserted) {
                    zlink::close_msg_frames (&raw_parts);
                    zlink::recv_tls_view::abort ();
                    errno = EPROTO;
                    return -1;
                }
                if (!reply_target_reservation.commit_locked ()) {
                    state->router_reply_targets.erase (published_reply_key);
                    zlink::close_msg_frames (&raw_parts);
                    zlink::recv_tls_view::abort ();
                    return -1;
                }
                published_reply_guard.arm_router (
                  state, &published_reply_key);
            }
        }
        for (size_t i = 0; i < start_index; ++i)
            zlink_msg_close (&raw_parts[i]);
    } else
        *request_seq_out_ = 0;

    metadata.source_rid = *source_rid;
    *source_node_rid_out_ = &metadata.source_rid;
    const int export_rc = export_router_payload_parts (
      raw_parts.data (), raw_parts.size (), start_index, parts_out_,
      part_count_out_);
    if (export_rc == 0)
        published_reply_guard.release ();
    return export_rc;
}

int recv_dealer_message_direct (
  const socket_handle_t &handle_,
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint8_t *message_type_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t **parts_out_,
  size_t *part_count_out_,
  int flags_,
  zlink_msg_t *terminal_part_out_,
  bool *terminal_part_returned_out_)
{
    if (!handle_.socket || !message_type_out_ || !request_seq_out_
        || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (terminal_part_returned_out_)
        *terminal_part_returned_out_ = false;

    const bool receive_terminal_direct =
      terminal_part_out_ && terminal_part_returned_out_
      && reinterpret_cast<zlink::msg_t *> (terminal_part_out_)->check ();
    zlink::msg_t current_storage;
    if (!receive_terminal_direct) {
        const int current_init_rc = current_storage.init ();
        errno_assert (current_init_rc == 0);
    }
    zlink::msg_t &current =
      receive_terminal_direct
        ? *reinterpret_cast<zlink::msg_t *> (terminal_part_out_)
        : current_storage;
    zlink::pipe_t *source_pipe = NULL;
    const int first_recv_rc = handle_.socket->recv_pipe (
      &current, &source_pipe, flags_);
    if (first_recv_rc != 0)
        return -1;

    if ((current.flags () & zlink::msg_t::more) == 0) {
        // A request envelope is multipart by contract. A terminal DEALER
        // frame is therefore an ordinary raw message and has no reply-target
        // or envelope state to own.
        int export_rc = 0;
        if (receive_terminal_direct) {
            *terminal_part_returned_out_ = true;
        } else {
            export_rc = zlink::recv_tls_view::export_single (
              reinterpret_cast<zlink_msg_t *> (&current), parts_out_,
              part_count_out_);
        }
        if (export_rc != 0)
            return -1;
        *message_type_out_ = ZLINK_DEALER_MESSAGE_RAW;
        *request_seq_out_ = 0;
        return 0;
    }

    request_reply_frame_buffer_t raw_parts;
    while (true) {
        const bool more = (current.flags () & zlink::msg_t::more) != 0;
        try {
#ifdef ZLINK_BUILD_TESTS
            if (raw_parts.size () == inline_request_reply_frame_capacity)
                test_throw_request_reply_allocation_failpoint (
                  request_reply_allocation_receive_spill);
#endif
            raw_parts.push_back (zlink_msg_t ());
        } catch (...) {
            zlink::close_msg_frames (&raw_parts);
            discard_received_message_tail (
              handle_.socket, &current, more, receive_terminal_direct);
            errno = ENOMEM;
            return -1;
        }
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
        const int followup_rc = handle_.socket->recv (
          &next, ZLINK_DONTWAIT);
        if (followup_rc != 0) {
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
    std::shared_ptr<socket_request_reply_state_t> request_state = state_;
    reply_target_publish_guard_t published_reply_guard;
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
        if (!request_state)
            request_state = find_or_create_request_reply_state (handle_);
        reply_target_reservation_t reply_target_reservation (request_state);
        if (!reply_target_reservation.acquire ()) {
            zlink::close_msg_frames (&raw_parts);
            return -1;
        }
        {
            std::lock_guard<std::mutex> lock (request_state->mutex);
            if (request_state->closing) {
                zlink::close_msg_frames (&raw_parts);
                errno = ETERM;
                return -1;
            }
            exported_seq = allocate_dealer_reply_token (request_state.get ());
            if (exported_seq == 0) {
                zlink::close_msg_frames (&raw_parts);
                return -1;
            }
            dealer_reply_target_t target;
            target.pipe = source_pipe;
            target.request_seq = envelope.request_seq;
            try {
                if (!request_state->dealer_reply_targets.emplace (
                      exported_seq, target).second) {
                    zlink::close_msg_frames (&raw_parts);
                    errno = EPROTO;
                    return -1;
                }
            } catch (...) {
                zlink::close_msg_frames (&raw_parts);
                errno = ENOMEM;
                return -1;
            }
            if (!reply_target_reservation.commit_locked ()) {
                request_state->dealer_reply_targets.erase (exported_seq);
                zlink::close_msg_frames (&raw_parts);
                return -1;
            }
            published_reply_guard.arm_dealer (request_state, exported_seq);
        }
        exported_type = ZLINK_DEALER_MESSAGE_REQUEST;
        start_index = zlink::request_reply::control_part_count;
        for (size_t i = 0; i < start_index; ++i)
            zlink_msg_close (&raw_parts[i]);
    }

    if (begin_payload_export (parts_out_, part_count_out_) != 0) {
        zlink::close_msg_frames (&raw_parts);
        return -1;
    }
    if (export_router_payload_parts (raw_parts.data (), raw_parts.size (), start_index,
                                     parts_out_, part_count_out_)
        != 0)
        return -1;
    published_reply_guard.release ();
    *message_type_out_ = exported_type;
    *request_seq_out_ = exported_seq;
    return 0;
}

int send_request_reply_message (const socket_handle_t &handle_,
                                const zlink_routing_id_t *peer_rid_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_,
                                uint8_t message_type_,
                                uint64_t request_seq_)
{
    if (!handle_.socket || !parts_ || part_count_ == 0 || request_seq_ == 0) {
        errno = EINVAL;
        return -1;
    }

    const bool routed = zlink::valid_routing_id (peer_rid_);
    std::shared_ptr<socket_request_reply_state_t> reply_state;
    pending_key_t reply_key;
    if (message_type_ != zlink::request_reply::request_type
        && handle_.socket->socket_type () == ZLINK_CORE_SOCKET_ROUTER
        && routed) {
        reply_state = find_request_reply_state (handle_);
        if (reply_state) {
            try {
#ifdef ZLINK_BUILD_TESTS
                test_throw_request_reply_allocation_failpoint (
                  request_reply_allocation_reply_key);
#endif
                reply_key.peer_rid = zlink::routing_id_key (peer_rid_);
            } catch (...) {
                zlink::request_reply::consume_send_frames_from (
                  parts_, 0, part_count_);
                errno = ENOMEM;
                return -1;
            }
            reply_key.request_seq = request_seq_;
        }
    }

    const size_t total_part_count = zlink::request_reply::control_part_count + part_count_;
    zlink_msg_t stack_combined[stack_request_reply_part_capacity];
    std::vector<zlink_msg_t> heap_combined;
    zlink_msg_t *combined =
      total_part_count <= stack_request_reply_part_capacity ? stack_combined : NULL;
    if (!combined) {
        try {
#ifdef ZLINK_BUILD_TESTS
            test_throw_request_reply_allocation_failpoint (
              request_reply_allocation_runtime_combined);
#endif
            heap_combined.resize (total_part_count);
        } catch (...) {
            zlink::request_reply::consume_send_frames_from (
              parts_, 0, part_count_);
            errno = ENOMEM;
            return -1;
        }
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
        if (handle_.socket->process_submit_commands () != 0) {
            const int saved_errno = errno;
            zlink::request_reply::close_built_parts (combined, total_part_count);
            errno = saved_errno;
            return -1;
        }
        zlink::pipe_t *application_pipe = NULL;
        bool target_taken = false;
        if (reply_state)
            target_taken = take_router_reply_target (
              reply_state, reply_key, &application_pipe);
        const int rc =
          send_completion_frames (handle_.socket, application_pipe, peer_rid_,
                                  combined, total_part_count);
        if (rc != 0) {
            if (target_taken)
                restore_router_reply_target (reply_state, reply_key);
            return -1;
        }
        if (target_taken)
            commit_router_reply_target (reply_state, reply_key);
        errno = 0;
        return 0;
    }

    router_mandatory_scope_t mandatory_scope;
    if (routed && mandatory_scope.arm (handle_) != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        errno = saved_errno;
        return -1;
    }

    const int rc =
      routed ? zlink::logical_multipart_send_routed (handle_.socket, peer_rid_, combined,
                                                     total_part_count, flags_)
             : zlink::logical_multipart_send (handle_.socket, combined, total_part_count,
                                              flags_);
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

    //  Both accessors hand back a pinned pipe: this reply path runs on an
    //  application thread while a second mailbox executor can be running the
    //  completion lane's pipe_terminated, which clears the transport-pair
    //  slot and then deallocates the pipe. Without the pin the write () below
    //  would lock a freed pipe_t::_out_sync. The pin is dropped on every exit
    //  from here on, so the loop breaks instead of returning.
    zlink::pipe_t *completion =
      application_pipe_
        ? socket_->completion_pipe_for_application (application_pipe_)
        : socket_->completion_pipe_for_peer (peer_rid_);
    if (!completion) {
        zlink::request_reply::consume_send_frames_from (
          parts_, 0, part_count_);
        errno = ENOTCONN;
        return -1;
    }
    int rc = 0;
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
            completion->rollback ();
            zlink::request_reply::consume_send_frames_from (
              parts_, i, part_count_);
            // Completion pipes do not expose application backpressure. A
            // failed write means the selected reply target disappeared while
            // the reply was being committed.
            errno = ENOTCONN;
            rc = -1;
            break;
        }
        const int init_rc = zlink_msg_init (&parts_[i]);
        errno_assert (init_rc == 0);
    }
    if (rc == 0)
        errno = 0;
    //  Dropping the pin can run the pipe destructor, so preserve the errno
    //  this function reports across it.
    const int reported_errno = errno;
    completion->release_lifetime_ref ();
    errno = reported_errno;
    return rc;
}

int send_completion_frames_for_transport_pair (
  zlink::socket_base_t *socket_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *parts_,
  size_t part_count_)
{
    if (!socket_ || !parts_ || part_count_ == 0
        || transport_pair_id_ == 0 || transport_pair_generation_ == 0) {
        errno = EINVAL;
        return -1;
    }
    if (socket_->process_submit_commands () != 0)
        return -1;

    zlink::pipe_t *completion = socket_->completion_pipe_for_transport_pair (
      transport_pair_id_, transport_pair_generation_);
    if (!completion) {
        zlink::request_reply::consume_send_frames_from (parts_, 0, part_count_);
        errno = ENOTCONN;
        return -1;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (i + 1 < part_count_)
            msg->set_flags (zlink::msg_t::more);
        else
            msg->reset_flags (zlink::msg_t::more);
        msg->set_transport_connection_id (completion->get_transport_connection_id ());
        const bool written = i + 1 < part_count_ ? completion->write (msg)
                                                 : completion->write_and_flush (msg);
        if (!written) {
            completion->rollback ();
            zlink::request_reply::consume_send_frames_from (parts_, i, part_count_);
            const int reported_errno = ENOTCONN;
            completion->release_lifetime_ref ();
            errno = reported_errno;
            return -1;
        }
        const int init_rc = zlink_msg_init (&parts_[i]);
        errno_assert (init_rc == 0);
    }
    completion->release_lifetime_ref ();
    errno = 0;
    return 0;
}
}
}
