/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/request_reply_frame_buffer_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/recv_tls_view.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
static void notify_reply_target_slots_released (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  size_t released_slots_)
{
    if (state_ && state_->socket && released_slots_ != 0)
        state_->socket->reply_target_slots_released (released_slots_);
}

class router_reply_target_reservation_t
{
  public:
    router_reply_target_reservation_t () : _active (false) {}

    void bind (const std::shared_ptr<socket_request_reply_state_t> &state_)
    {
        zlink_assert (!_active);
        _state = state_;
    }

    bool active () const { return _active; }

    void release ()
    {
        if (!_active)
            return;
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            zlink_assert (_state->reply_target_reservations > 0);
            zlink_assert (_state->reply_target_slots > 0);
            --_state->reply_target_reservations;
            --_state->reply_target_slots;
            _active = false;
        }
        notify_reply_target_slots_released (_state, 1);
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

    ~router_reply_target_reservation_t ()
    {
        release ();
    }

  private:
    std::shared_ptr<socket_request_reply_state_t> _state;
    bool _active;
};

static int acquire_router_reply_target_reservation (
  const socket_handle_t &handle_,
  std::shared_ptr<socket_request_reply_state_t> *state_,
  router_reply_target_reservation_t *reservation_)
{
    if (!state_ || !reservation_) {
        errno = EFAULT;
        return -1;
    }
    if (reservation_->active ())
        return 0;

    // Re-read the bridge only after this receive attempt owns the socket. A
    // competing first reader may have installed it while this call waited for
    // the prior whole-record transaction.
    try {
#ifdef ZLINK_BUILD_TESTS
        if (!*state_)
            test_throw_request_reply_allocation_failpoint (
              request_reply_allocation_lazy_state_create);
#endif
        *state_ = find_or_create_request_reply_state (handle_);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    if (!*state_)
        return -1;

    reservation_->bind (*state_);
    return reservation_->acquire () ? 0 : -1;
}

struct router_reply_target_receive_admission_t
{
    router_reply_target_receive_admission_t (
      const socket_handle_t &handle_,
      std::shared_ptr<socket_request_reply_state_t> *state_out_,
      router_reply_target_reservation_t *reservation_) :
        handle (&handle_),
        state_out (state_out_),
        reservation (reservation_),
        incoming_request (false)
    {
    }

    static int prepare (void *userdata_)
    {
        router_reply_target_receive_admission_t *self =
          static_cast<router_reply_target_receive_admission_t *> (userdata_);
        if (!self || !self->incoming_request || !self->handle
            || !self->state_out || !self->reservation)
            return 0;
        return acquire_router_reply_target_reservation (
          *self->handle, self->state_out, self->reservation);
    }

    static void rollback (void *userdata_)
    {
        router_reply_target_receive_admission_t *self =
          static_cast<router_reply_target_receive_admission_t *> (userdata_);
        if (self && self->reservation)
            self->reservation->release ();
    }

    // Admission hooks run synchronously inside the enclosing receive, so
    // borrowing avoids a public-handle retain/release on every record.
    const socket_handle_t *handle;
    std::shared_ptr<socket_request_reply_state_t> *state_out;
    router_reply_target_reservation_t *reservation;
    bool incoming_request;

    void set_incoming_request (bool incoming_request_)
    {
        incoming_request = incoming_request_;
    }
};

class public_part_delivery_hold_failure_guard_t
{
  public:
    explicit public_part_delivery_hold_failure_guard_t (
      socket_base_t *socket_) :
        socket (socket_),
        armed (false)
    {
    }

    ~public_part_delivery_hold_failure_guard_t ()
    {
        if (armed && socket)
            socket->end_public_part_receive_delivery_hold ();
    }

    void arm () { armed = true; }
    void release () { armed = false; }

  private:
    socket_base_t *socket;
    bool armed;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (
      public_part_delivery_hold_failure_guard_t)
};

struct public_part_delivery_hold_admission_t
{
    public_part_delivery_hold_admission_t (
      socket_base_t *socket_,
      public_part_delivery_hold_failure_guard_t *failure_guard_) :
        socket (socket_),
        failure_guard (failure_guard_)
    {
    }

    static int prepare (void *userdata_)
    {
        public_part_delivery_hold_admission_t *self =
          static_cast<public_part_delivery_hold_admission_t *> (userdata_);
        if (!self || !self->socket) {
            errno = EFAULT;
            return -1;
        }
        const int rc =
          self->socket->begin_public_part_receive_delivery_hold ();
        if (rc == 0 && self->failure_guard)
            self->failure_guard->arm ();
        return rc;
    }

    static void rollback (void *userdata_)
    {
        public_part_delivery_hold_admission_t *self =
          static_cast<public_part_delivery_hold_admission_t *> (userdata_);
        if (self && self->socket) {
            self->socket->end_public_part_receive_delivery_hold (true);
            if (self->failure_guard)
                self->failure_guard->release ();
        }
    }

    socket_base_t *socket;
    public_part_delivery_hold_failure_guard_t *failure_guard;
};

typedef reply_target_store_t<router_reply_target_t>
  router_reply_target_store_t;
typedef reply_target_store_t<dealer_reply_target_t>
  dealer_reply_target_store_t;

static bool router_reply_target_matches_rid (
  const router_reply_target_t &target_, const zlink_routing_id_t *rid_)
{
    return !target_.peer_rid.empty () && zlink::valid_routing_id (rid_)
           && target_.peer_rid.size () == rid_->size
           && memcmp (target_.peer_rid.data (), rid_->data, rid_->size) == 0;
}

static router_reply_alias_key_t make_router_reply_alias_key (
    const router_reply_target_t &target_)
{
    router_reply_alias_key_t key;
    key.pipe = target_.source_pipe_identity;
    key.source_peer_socket_type = target_.source_peer_socket_type;
    key.transport_pair_id = target_.transport_pair_id;
    key.transport_pair_generation = target_.transport_pair_generation;
    key.wire_request_seq = target_.wire_request_seq;
    return key;
}

static size_t router_reply_alias_bucket_for (
  const router_reply_alias_key_t &key_)
{
    return router_reply_alias_key_hash_t () (key_)
           & static_cast<size_t> (
             socket_request_reply_state_t::router_reply_alias_bucket_count
             - 1);
}

static bool router_reply_alias_exists_locked (
  const socket_request_reply_state_t *state_,
  const router_reply_alias_key_t &key_)
{
    const size_t bucket = router_reply_alias_bucket_for (key_);
    const router_reply_target_store_t::node_t *node =
      state_->router_reply_alias_buckets[bucket];
    while (node) {
        if (make_router_reply_alias_key (node->second) == key_)
            return true;
        node = node->alias_next;
    }
    return false;
}

static void index_router_reply_alias_locked (
  socket_request_reply_state_t *state_,
  router_reply_target_store_t::iterator target_)
{
    router_reply_target_store_t::node_t *const node = &*target_;
    const size_t bucket =
      router_reply_alias_bucket_for (
        make_router_reply_alias_key (node->second));
    node->alias_next = state_->router_reply_alias_buckets[bucket];
    state_->router_reply_alias_buckets[bucket] = node;
}

static router_reply_target_store_t::iterator erase_router_reply_target_locked (
  socket_request_reply_state_t *state_,
  router_reply_target_store_t::iterator target_)
{
    zlink_assert (state_);
    zlink_assert (target_ != state_->router_reply_targets.end ());
    router_reply_target_store_t::node_t *const node = &*target_;
    const size_t bucket =
      router_reply_alias_bucket_for (
        make_router_reply_alias_key (node->second));
    router_reply_target_store_t::node_t **link =
      &state_->router_reply_alias_buckets[bucket];
    while (*link && *link != node)
        link = &(*link)->alias_next;
    zlink_assert (*link == node);
    if (*link == node)
        *link = node->alias_next;
    node->alias_next = NULL;
    return state_->router_reply_targets.erase (target_);
}

void clear_router_reply_targets_locked (socket_request_reply_state_t *state_)
{
    if (!state_)
        return;
    state_->router_reply_targets.clear ();
    memset (state_->router_reply_alias_buckets, 0,
            sizeof (state_->router_reply_alias_buckets));
}

class router_reply_target_publish_guard_t
{
  public:
    router_reply_target_publish_guard_t () :
        _router_token (0),
        _active (false)
    {
    }

    ~router_reply_target_publish_guard_t ()
    {
        if (!_active || !_state)
            return;

        bool erased = false;
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            router_reply_target_store_t::iterator it =
              _state->router_reply_targets.find (_router_token);
            if (it != _state->router_reply_targets.end ()) {
                zlink_assert (!it->second.checked_out);
                if (!it->second.checked_out) {
                    erase_router_reply_target_locked (_state.get (), it);
                    erased = true;
                }
            }

            // Close clears published targets and recomputes the slot count
            // while holding the same mutex. A missing node therefore means
            // close already released this slot.
            if (erased) {
                zlink_assert (_state->reply_target_slots > 0);
                --_state->reply_target_slots;
            }
        }
        if (erased)
            notify_reply_target_slots_released (_state, 1);
    }

    void arm_router (const std::shared_ptr<socket_request_reply_state_t> &state_,
                     uint64_t token_)
    {
        _state = state_;
        _router_token = token_;
        _active = state_ && token_ != 0;
    }

    void release () { _active = false; }

  private:
    std::shared_ptr<socket_request_reply_state_t> _state;
    uint64_t _router_token;
    bool _active;
};

class received_pipe_pin_t
{
  public:
    explicit received_pipe_pin_t (zlink::pipe_t *pipe_) : _pipe (pipe_) {}

    ~received_pipe_pin_t ()
    {
        if (_pipe)
            _pipe->release_lifetime_ref ();
    }

  private:
    zlink::pipe_t *_pipe;
};

// Runs while pipe_t still owns the queued first application frame. Only
// metadata and multipart records need whole-record ownership; raw terminal
// frames remain on the lock-free public receive path.
class routed_receive_pre_admission_t
{
  public:
    routed_receive_pre_admission_t (
      zlink::socket_receive_record_scope_t *scope_,
      router_reply_target_receive_admission_t *admission_,
      zlink::socket_base_t *socket_) :
        _scope (scope_), _admission (admission_), _socket (socket_),
        _pinned_pipe (NULL)
    {
    }

    ~routed_receive_pre_admission_t ()
    {
        if (_pinned_pipe)
            _pinned_pipe->release_lifetime_ref ();
    }

    static int admit (zlink::pipe_t *pipe_, const zlink::msg_t &msg_,
                      void *userdata_)
    {
        routed_receive_pre_admission_t *const self =
          static_cast<routed_receive_pre_admission_t *> (userdata_);
        if (msg_.is_routing_id ())
            return 0;
        uint8_t message_type = zlink::zmp_kind_data;
        uint64_t sequence = 0;
        const bool has_metadata =
          zlink::request_reply::read_request_reply_metadata (
            reinterpret_cast<const zlink_msg_t *> (&msg_), &message_type,
            &sequence);
        const bool is_request = has_metadata
                                && message_type
                                     == zlink::request_reply::request_type
                                && sequence != 0;
        const bool needs_record = has_metadata
                                  || (msg_.flags () & zlink::msg_t::more) != 0;
        if (!needs_record)
            return 0;

        // Raw multipart still needs the socket-wide record fence, but it does
        // not retain a source pointer after this call. Metadata can publish a
        // reply target and therefore keeps the source-pipe lifetime pin.
        if (has_metadata) {
            if (!pipe_ || !self->_socket
                || !self->_socket->retain_received_source_pipe_ref (pipe_)) {
                errno = EPROTO;
                return -1;
            }
            self->_pinned_pipe = pipe_;
        }
        self->_admission->set_incoming_request (is_request);
        if (self->_scope->acquire_before_frame () != 0) {
            if (self->_pinned_pipe) {
                self->_pinned_pipe->release_lifetime_ref ();
                self->_pinned_pipe = NULL;
            }
            return errno == ENOMEM
                     ? zlink::pipe_t::read_admission_reject_consume
                     : -1;
        }
        return 0;
    }

    zlink::pipe_t *release_pinned_pipe ()
    {
        zlink::pipe_t *const result = _pinned_pipe;
        _pinned_pipe = NULL;
        return result;
    }

  private:
    zlink::socket_receive_record_scope_t *_scope;
    router_reply_target_receive_admission_t *_admission;
    zlink::socket_base_t *_socket;
    zlink::pipe_t *_pinned_pipe;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (routed_receive_pre_admission_t)
};

static void discard_received_message_tail (
  zlink::socket_base_t *socket_, zlink::msg_t *current_,
  bool current_has_more_, bool reinitialize_current_,
  zlink::socket_receive_record_scope_t &record_scope_)
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
        if (socket_->recv_record_continuation (&next, record_scope_) != 0) {
            const int close_rc = next.close ();
            errno_assert (close_rc == 0);
            return;
        }
        more = (next.flags () & zlink::msg_t::more) != 0;
        const int close_rc = next.close ();
        errno_assert (close_rc == 0);
    }
}

static int collect_multipart_payload_parts (
  zlink::socket_base_t *socket_, zlink::msg_t *current_,
  bool reinitialize_current_,
  zlink::socket_receive_record_scope_t &record_scope_,
  zlink::pipe_t *source_pipe_, request_reply_frame_buffer_t *parts_)
{
    if (!socket_ || !current_ || !parts_) {
        errno = EFAULT;
        return -1;
    }

    while (true) {
        const bool more = (current_->flags () & zlink::msg_t::more) != 0;
        try {
#ifdef ZLINK_BUILD_TESTS
            if (parts_->size () == inline_request_reply_frame_capacity)
                test_throw_request_reply_allocation_failpoint (
                  request_reply_allocation_receive_spill);
#endif
            parts_->append_uninitialized ();
        } catch (...) {
            close_request_reply_frame_buffer (parts_);
            discard_received_message_tail (
              socket_, current_, more, reinitialize_current_, record_scope_);
            errno = ENOMEM;
            return -1;
        }

        zlink_msg_init (&parts_->back ());
        if (reinterpret_cast<zlink::msg_t *> (&parts_->back ())
              ->move (*current_)
            != 0) {
            close_request_reply_frame_buffer (parts_);
            errno = EFAULT;
            return -1;
        }
        if (!more)
            return 0;

        zlink::msg_t next;
        const int init_rc = next.init ();
        errno_assert (init_rc == 0);
        if (socket_->recv_record_continuation (&next, record_scope_) != 0) {
            const int saved_errno = errno;
            const int close_rc = next.close ();
            errno_assert (close_rc == 0);
            close_request_reply_frame_buffer (parts_);
            errno = saved_errno;
            return -1;
        }

        uint8_t later_kind = zlink::zmp_kind_data;
        uint64_t later_sequence = 0;
        if (zlink::request_reply::read_request_reply_metadata (
              reinterpret_cast<const zlink_msg_t *> (&next), &later_kind,
              &later_sequence)) {
            const bool next_has_more =
              (next.flags () & zlink::msg_t::more) != 0;
            discard_received_message_tail (
              socket_, &next, next_has_more, false, record_scope_);
            close_request_reply_frame_buffer (parts_);
            if (source_pipe_)
                source_pipe_->terminate (false);
            errno = EPROTO;
            return -1;
        }

        if (current_->move (next) != 0) {
            const int close_rc = next.close ();
            errno_assert (close_rc == 0);
            close_request_reply_frame_buffer (parts_);
            errno = EFAULT;
            return -1;
        }
    }
}

static inline int export_single_payload (zlink::msg_t *part_,
                                         zlink_msg_t **parts_out_,
                                         size_t *part_count_out_)
{
    *parts_out_ = NULL;
    *part_count_out_ = 0;

    int rc = -1;
    try {
        rc = zlink::recv_tls_view::begin (parts_out_, part_count_out_);
        if (rc == 0)
            rc = zlink::recv_tls_view::export_single (
              reinterpret_cast<zlink_msg_t *> (part_), parts_out_,
              part_count_out_);
    } catch (...) {
        errno = ENOMEM;
    }
    if (rc == 0)
        return 0;

    const int saved_errno = errno;
    zlink::request_reply::consume_send_frame (
      reinterpret_cast<zlink_msg_t *> (part_));
    *parts_out_ = NULL;
    *part_count_out_ = 0;
    errno = saved_errno;
    return -1;
}

static uint64_t allocate_router_reply_token_locked (
  socket_request_reply_state_t *state_)
{
    if (!state_) {
        errno = EFAULT;
        return 0;
    }

    // Reply tokens are socket-local monotonic capabilities and are never
    // recycled before close. A wrapped sequence is permanently exhausted.
    if (state_->router_next_reply_token == 0) {
        errno = EOVERFLOW;
        return 0;
    }
    const uint64_t token = state_->router_next_reply_token++;
    zlink_assert (token != 0);
    return token;
}

void forget_dealer_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_)
{
    if (!state_ || !application_pipe_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    size_t erased = 0;
    for (dealer_reply_target_store_t::iterator it =
           state_->dealer_reply_targets.begin ();
         it != state_->dealer_reply_targets.end ();) {
        if (it->second.pipe == application_pipe_) {
            if (it->second.checked_out) {
                // The reply submit owns a lifetime pin. Make restore discard
                // the checked-out target after it observes the detached pair.
                it->second.pipe = NULL;
                ++it;
            } else {
                it = state_->dealer_reply_targets.erase (it);
                ++erased;
            }
        } else
            ++it;
    }
    zlink_assert (state_->reply_target_slots >= erased);
    state_->reply_target_slots -= erased;
}

bool take_router_reply_target_locked (
  socket_request_reply_state_t *state_, uint64_t request_token_,
  const zlink_routing_id_t *peer_rid_,
  router_reply_target_t *target_out_)
{
    if (!state_ || request_token_ == 0 || !target_out_
        || !zlink::valid_routing_id (peer_rid_))
        return false;

    router_reply_target_store_t::iterator it =
      state_->router_reply_targets.find (request_token_);
    if (it == state_->router_reply_targets.end () || it->second.checked_out
        || it->second.revoked
        || !router_reply_target_matches_rid (it->second, peer_rid_)
        || it->second.wire_request_seq == 0
        || (it->second.source_peer_socket_type != ZLINK_CORE_SOCKET_DEALER
            && it->second.source_peer_socket_type
                 != ZLINK_CORE_SOCKET_ROUTER))
        return false;
    if (it->second.pipe
        && (it->second.pipe->get_transport_lane ()
              != transport_lane_application
            || it->second.pipe->get_peer_socket_type ()
                 != it->second.source_peer_socket_type
            || it->second.pipe->get_transport_pair_id ()
              != it->second.transport_pair_id
            || it->second.pipe->get_transport_pair_generation ()
                 != it->second.transport_pair_generation
            || !it->second.pipe->retain_lifetime_ref ()))
        it->second.pipe = NULL;
    *target_out_ = it->second;
    it->second.checked_out = true;
    zlink_assert (state_->reply_target_slots > 0);
    ++state_->reply_target_checkouts;
    return true;
}

static bool restore_router_reply_target_locked (
  socket_request_reply_state_t *state_, uint64_t request_token_)
{
    zlink_assert (state_);
    zlink_assert (state_->reply_target_checkouts > 0);
    --state_->reply_target_checkouts;
    router_reply_target_store_t::iterator it =
      state_->router_reply_targets.find (request_token_);
    if (state_->closing || it == state_->router_reply_targets.end ()
        || it->second.revoked) {
        zlink_assert (state_->closing
                      || it != state_->router_reply_targets.end ());
        zlink_assert (state_->reply_target_slots > 0);
        if (it != state_->router_reply_targets.end ())
            erase_router_reply_target_locked (state_, it);
        --state_->reply_target_slots;
        return true;
    }
    zlink_assert (it->second.checked_out);
    it->second.checked_out = false;
    return false;
}

void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t request_token_)
{
    if (!state_)
        return;
    bool released = false;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        released = restore_router_reply_target_locked (
          state_.get (), request_token_);
    }
    if (released)
        notify_reply_target_slots_released (state_, 1);
}

void abandon_public_router_reply_sequence (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t expected_token_)
{
    if (!state_)
        return;

    bool active = false;
    bool released = false;
    pipe_t *target_pipe = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->public_router_reply_active
            && (expected_token_ == 0
                || state_->public_router_reply_token == expected_token_)) {
            state_->public_router_reply_checkout_token.store (
              0, std::memory_order_release);
            active = true;
            target_pipe = state_->public_router_reply_target.pipe;
            released = restore_router_reply_target_locked (
              state_.get (), state_->public_router_reply_token);
            state_->public_router_reply_active = false;
            state_->public_router_reply_owner = std::thread::id ();
            state_->public_router_reply_token = 0;
            state_->public_router_reply_target = router_reply_target_t ();
        }
    }
    if (!active)
        return;
    if (target_pipe)
        target_pipe->release_lifetime_ref ();
    if (released)
        notify_reply_target_slots_released (state_, 1);
}

static void commit_router_reply_target_locked (
  socket_request_reply_state_t *state_, uint64_t request_token_)
{
    zlink_assert (state_);
    zlink_assert (state_->reply_target_checkouts > 0);
    zlink_assert (state_->reply_target_slots > 0);
    router_reply_target_store_t::iterator it =
      state_->router_reply_targets.find (request_token_);
    if (it != state_->router_reply_targets.end ()) {
        zlink_assert (it->second.checked_out);
        erase_router_reply_target_locked (state_, it);
    } else
        zlink_assert (state_->closing);
    --state_->reply_target_checkouts;
    --state_->reply_target_slots;
}

void commit_public_router_reply_sequence (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t expected_token_)
{
    if (!state_)
        return;
    bool active = false;
    pipe_t *target_pipe = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->public_router_reply_active
            && state_->public_router_reply_token == expected_token_) {
            state_->public_router_reply_checkout_token.store (
              0, std::memory_order_release);
            active = true;
            target_pipe = state_->public_router_reply_target.pipe;
            commit_router_reply_target_locked (
              state_.get (), state_->public_router_reply_token);
            state_->public_router_reply_active = false;
            state_->public_router_reply_owner = std::thread::id ();
            state_->public_router_reply_token = 0;
            state_->public_router_reply_target = router_reply_target_t ();
        }
    }
    if (!active)
        return;
    if (target_pipe)
        target_pipe->release_lifetime_ref ();
    notify_reply_target_slots_released (state_, 1);
}

void revoke_router_reply_target (const socket_handle_t &handle_,
                                 const zlink_routing_id_t *peer_rid_,
                                 uint64_t request_seq_)
{
    if (!handle_.socket || !peer_rid_ || request_seq_ == 0)
        return;

    const std::shared_ptr<socket_request_reply_state_t> state =
      find_request_reply_state (handle_);
    if (!state)
        return;

    bool released = false;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        router_reply_target_store_t::iterator it =
          state->router_reply_targets.find (request_seq_);
        if (it != state->router_reply_targets.end ()
            && router_reply_target_matches_rid (it->second, peer_rid_)) {
            zlink_assert (!it->second.checked_out);
            if (!it->second.checked_out) {
                erase_router_reply_target_locked (state.get (), it);
                zlink_assert (state->reply_target_slots > 0);
                --state->reply_target_slots;
                released = true;
            }
        }
    }
    if (released)
        notify_reply_target_slots_released (state, 1);
}

void revoke_router_reply_targets_for_rid (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const zlink_routing_id_t *peer_rid_)
{
    if (!state_ || !zlink::valid_routing_id (peer_rid_))
        return;

    pipe_t *active_pipe_pin = NULL;
    size_t released = 0;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->public_router_reply_active
            && router_reply_target_matches_rid (
              state_->public_router_reply_target, peer_rid_)) {
            state_->public_router_reply_checkout_token.store (
              0, std::memory_order_release);
            const uint64_t active_token = state_->public_router_reply_token;
            active_pipe_pin = state_->public_router_reply_target.pipe;
            state_->public_router_reply_active = false;
            state_->public_router_reply_owner = std::thread::id ();
            state_->public_router_reply_token = 0;
            state_->public_router_reply_target = router_reply_target_t ();

            router_reply_target_store_t::iterator active =
              state_->router_reply_targets.find (active_token);
            if (active != state_->router_reply_targets.end ()) {
                zlink_assert (active->second.checked_out);
                erase_router_reply_target_locked (state_.get (), active);
                zlink_assert (state_->reply_target_checkouts > 0);
                zlink_assert (state_->reply_target_slots > 0);
                --state_->reply_target_checkouts;
                --state_->reply_target_slots;
                ++released;
            }
        }

        for (router_reply_target_store_t::iterator it =
               state_->router_reply_targets.begin ();
             it != state_->router_reply_targets.end ();) {
            if (!router_reply_target_matches_rid (it->second, peer_rid_)) {
                ++it;
                continue;
            }
            if (it->second.checked_out) {
                // A concurrent submit owns its lifetime pin. Mark the entry
                // invalid so its restore path releases the checkout and slot;
                // never expose it for another sequence after logical removal.
                it->second.pipe = NULL;
                it->second.revoked = true;
                ++it;
                continue;
            }
            it = erase_router_reply_target_locked (state_.get (), it);
            zlink_assert (state_->reply_target_slots > 0);
            --state_->reply_target_slots;
            ++released;
        }
    }

    if (active_pipe_pin)
        active_pipe_pin->release_lifetime_ref ();
    notify_reply_target_slots_released (state_, released);
}

void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_)
{
    if (!state_ || !application_pipe_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    for (router_reply_target_store_t::iterator it =
           state_->router_reply_targets.begin ();
         it != state_->router_reply_targets.end (); ++it) {
        if (it->second.pipe == application_pipe_)
            // Preserve the logical token and wire correlation. A later FINAL
            // resolves the then-current completion route for this RID.
            it->second.pipe = NULL;
    }
}

static int export_payload_parts (zlink_msg_t *parts_,
                                 size_t part_count_,
                                 zlink_msg_t **parts_out_,
                                 size_t *part_count_out_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EPROTO;
        return -1;
    }

    *parts_out_ = NULL;
    *part_count_out_ = 0;

    int begin_rc = -1;
    try {
        begin_rc = zlink::recv_tls_view::begin (
          parts_out_, part_count_out_);
    } catch (...) {
        errno = ENOMEM;
    }
    if (begin_rc != 0) {
        const int saved_errno = errno;
        zlink::request_reply::consume_send_frames_from (
          parts_, 0, part_count_);
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i < part_count_; ++i) {
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
                                uint64_t *reply_token_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_,
                                bool *terminal_part_returned_out_,
                                uint64_t *transport_pair_id_out_,
                                uint64_t *transport_pair_generation_out_)
{
    if (!handle_.socket || !source_node_rid_out_ || !reply_token_out_ || !parts_out_
        || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (terminal_part_returned_out_)
        *terminal_part_returned_out_ = false;

    // Capacity admission runs only after this receive attempt owns the socket,
    // and before xrecv consumes the first frame. The admission hook also
    // refreshes a state pointer captured before a competing record installed
    // the lazy request/reply bridge.
    zlink::socket_receive_record_scope_t receive_record_scope;
    std::shared_ptr<socket_request_reply_state_t> state =
      handle_.socket->request_reply_state ();
    router_reply_target_reservation_t reply_target_reservation;
    router_reply_target_receive_admission_t receive_admission (
      handle_, &state, &reply_target_reservation);
    receive_record_scope.set_admission (
      &router_reply_target_receive_admission_t::prepare,
      &router_reply_target_receive_admission_t::rollback,
      &receive_admission);
    routed_receive_pre_admission_t pre_admission (&receive_record_scope,
                                                   &receive_admission,
                                                   handle_.socket);

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
    zlink_routing_id_t source_rid_storage;
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    uint64_t route_binding_token = 0;
    zlink_routing_id_t *const source_rid = &source_rid_storage;
    zlink::pipe_t *source_pipe = NULL;
    const int first_recv_rc = handle_.socket->recv_routed (
      &current, source_rid, flags_, NULL, &source_pipe, false,
      &transport_pair_id, &transport_pair_generation,
      &route_binding_token,
      &receive_record_scope, &routed_receive_pre_admission_t::admit,
      &pre_admission);
    received_pipe_pin_t source_pipe_pin (pre_admission.release_pinned_pipe ());
    if (first_recv_rc != 0)
        return -1;

    const bool first_has_more =
      (current.flags () & zlink::msg_t::more) != 0;
    uint8_t message_type = zlink::zmp_kind_data;
    uint64_t wire_sequence = 0;
    const bool has_request_reply_metadata =
      zlink::request_reply::read_request_reply_metadata (
        reinterpret_cast<const zlink_msg_t *> (&current), &message_type,
        &wire_sequence);
    const bool is_request =
      has_request_reply_metadata
      && message_type == zlink::request_reply::request_type
      && wire_sequence != 0;
    if (has_request_reply_metadata && !is_request) {
        discard_received_message_tail (
          handle_.socket, &current, first_has_more, receive_terminal_direct,
          receive_record_scope);
        if (source_pipe)
            source_pipe->terminate (false);
        errno = EPROTO;
        return -1;
    }

    if (is_request && !source_pipe) {
        discard_received_message_tail (
          handle_.socket, &current, first_has_more, receive_terminal_direct,
          receive_record_scope);
        errno = EPROTO;
        return -1;
    }

    if (is_request
        && acquire_router_reply_target_reservation (
             handle_, &state, &reply_target_reservation)
             != 0) {
        const int saved_errno = errno;
        discard_received_message_tail (
          handle_.socket, &current, first_has_more, receive_terminal_direct,
          receive_record_scope);
        errno = saved_errno;
        return -1;
    }

    fixed_routing_id_key_t published_reply_rid;
    uint64_t published_reply_token = 0;
    if (is_request) {
        published_reply_rid.assign (source_rid->data, source_rid->size);
        // The wire sequence remains private transport correlation. A distinct
        // monotonic socket-local capability is allocated below before the
        // REQUEST is made visible to application receive.
    }

    // Metadata is an internal transport/runtime property. From this point the
    // wire sequence and kind live in local state and no public Message may
    // retain them.
    current.reset_request_reply_metadata ();

    request_reply_frame_buffer_t raw_parts;
    if (first_has_more
        && collect_multipart_payload_parts (
             handle_.socket, &current, receive_terminal_direct,
             receive_record_scope, source_pipe, &raw_parts)
             != 0)
        return -1;

    router_reply_target_publish_guard_t published_reply_guard;
    if (is_request) {
        int publish_error = 0;
        bool duplicate_sequence = false;
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            if (state->closing)
                publish_error = ETERM;
            else if (!source_pipe->is_lifecycle_active ())
                publish_error = ECONNABORTED;
            else {
                router_reply_target_t target;
                target.pipe = source_pipe;
                target.source_pipe_identity = source_pipe;
                target.source_peer_socket_type =
                  source_pipe->get_peer_socket_type ();
                target.wire_request_seq = wire_sequence;
                target.transport_pair_id = transport_pair_id;
                target.transport_pair_generation =
                  transport_pair_generation;
                target.route_binding_token = route_binding_token;
                bool inserted = false;
                const router_reply_alias_key_t alias_key =
                  make_router_reply_alias_key (target);
                if (router_reply_alias_exists_locked (state.get (),
                                                      alias_key)) {
                    publish_error = EPROTO;
                    duplicate_sequence = true;
                }
                if (!publish_error
                    && (published_reply_token =
                          allocate_router_reply_token_locked (state.get ()))
                         == 0)
                    publish_error = errno;
                if (!publish_error) {
                    target.peer_rid = published_reply_rid;
                    const std::pair<router_reply_target_store_t::iterator,
                                    bool>
                      insertion = state->router_reply_targets.emplace (
                        published_reply_token, target);
                    inserted = insertion.second;
                    if (inserted)
                        index_router_reply_alias_locked (state.get (),
                                                         insertion.first);
                    if (!inserted && (errno == ENOMEM || errno == EAGAIN))
                        publish_error = errno;
                }
                if (!publish_error && !inserted)
                    publish_error = EAGAIN;
                if (!publish_error
                    && !reply_target_reservation.commit_locked ()) {
                    publish_error = errno;
                    router_reply_target_store_t::iterator published =
                      state->router_reply_targets.find (published_reply_token);
                    if (published != state->router_reply_targets.end ())
                        erase_router_reply_target_locked (state.get (),
                                                          published);
                }
                if (!publish_error)
                    published_reply_guard.arm_router (
                      state, published_reply_token);
            }
        }
        if (publish_error) {
            if (first_has_more)
                close_request_reply_frame_buffer (&raw_parts);
            else
                discard_received_message_tail (
                  handle_.socket, &current, false,
                  receive_terminal_direct, receive_record_scope);
            if (duplicate_sequence && source_pipe)
                source_pipe->terminate (false);
            errno = publish_error;
            return -1;
        }
    }

    handle_.socket->store_last_recv_source_rid (source_rid);
    *source_node_rid_out_ = handle_.socket->last_recv_source_rid_view ();
    *reply_token_out_ = is_request ? published_reply_token : 0;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = transport_pair_id;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ = transport_pair_generation;

    int export_rc = 0;
    if (!first_has_more) {
        if (receive_terminal_direct) {
            *terminal_part_returned_out_ = true;
        } else {
            export_rc = export_single_payload (
              &current, parts_out_, part_count_out_);
        }
    } else {
        export_rc = export_payload_parts (
          raw_parts.data (), raw_parts.size (), parts_out_, part_count_out_);
    }
    if (export_rc == 0)
        published_reply_guard.release ();
    return export_rc;
}

int recv_dealer_message_direct (
  const socket_handle_t &handle_,
  zlink_msg_t **parts_out_,
  size_t *part_count_out_,
  int flags_,
  zlink_msg_t *terminal_part_out_,
  bool *terminal_part_returned_out_, bool public_part_receive_,
  bool *public_part_delivery_hold_out_)
{
    if (!handle_.socket || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (terminal_part_returned_out_)
        *terminal_part_returned_out_ = false;
    if (public_part_delivery_hold_out_)
        *public_part_delivery_hold_out_ = false;
    if (public_part_receive_ && !public_part_delivery_hold_out_) {
        errno = EFAULT;
        return -1;
    }

    //  The record scope must release receive.sync before an error guard wakes
    //  the suppressed head, so declare the guard first (destruction is
    //  reverse-order). A successful call transfers the exact hold outward.
    public_part_delivery_hold_failure_guard_t public_delivery_hold_guard (
      handle_.socket);
    zlink::socket_receive_record_scope_t receive_record_scope;
    public_part_delivery_hold_admission_t public_delivery_hold_admission (
      handle_.socket, &public_delivery_hold_guard);
    if (public_part_receive_) {
        receive_record_scope.set_admission (
          &public_part_delivery_hold_admission_t::prepare,
          &public_part_delivery_hold_admission_t::rollback,
          &public_delivery_hold_admission);
    }

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
      &current, &source_pipe, flags_, true, &receive_record_scope);
    received_pipe_pin_t source_pipe_pin (source_pipe);
    if (first_recv_rc != 0)
        return -1;
    if (public_part_receive_)
        handle_.socket->bind_public_part_receive_delivery_hold (source_pipe);

    const bool first_has_more =
      (current.flags () & zlink::msg_t::more) != 0;
    current.reset_request_reply_metadata ();

    request_reply_frame_buffer_t raw_parts;
    if (first_has_more
        && collect_multipart_payload_parts (
             handle_.socket, &current, receive_terminal_direct,
             receive_record_scope, source_pipe, &raw_parts)
             != 0)
        return -1;

    int export_rc = 0;
    if (!first_has_more) {
        if (receive_terminal_direct) {
            *terminal_part_returned_out_ = true;
        } else {
            export_rc = export_single_payload (
              &current, parts_out_, part_count_out_);
        }
    } else {
        export_rc = export_payload_parts (
          raw_parts.data (), raw_parts.size (), parts_out_, part_count_out_);
    }
    if (export_rc != 0)
        return -1;
    if (public_part_receive_) {
        *public_part_delivery_hold_out_ = true;
        public_delivery_hold_guard.release ();
    }
    return 0;
}

zlink::pipe_t *retain_reply_transport_pipe (
  zlink::socket_base_t *socket_, const router_reply_target_t &target_,
  const zlink_routing_id_t *peer_rid_)
{
    if (!socket_ || !zlink::valid_routing_id (peer_rid_)
        || target_.transport_pair_id == 0
        || target_.transport_pair_generation == 0
        || (target_.source_peer_socket_type != ZLINK_CORE_SOCKET_DEALER
            && target_.source_peer_socket_type != ZLINK_CORE_SOCKET_ROUTER)) {
        errno = EPROTO;
        return NULL;
    }

    const transport_lane_t lane =
      target_.source_peer_socket_type == ZLINK_CORE_SOCKET_DEALER
        ? transport_lane_application
        : transport_lane_completion;
    // A checked-out reply target already pins the Application pipe that
    // delivered the request. In the overwhelmingly common no-reconnect case,
    // validate that cached route in place and avoid resolving the same RID
    // through the ROUTER map (and then the pair table) for every reply.
    // Route publication invalidates the captured binding token before
    // handover removes or rewrites the old route.
    zlink::pipe_t *const cached_application = target_.pipe;
    if (cached_application
        && cached_application->get_transport_lane ()
             == transport_lane_application
        && target_.route_binding_token != 0
        && cached_application->router_route_binding_token ()
             == target_.route_binding_token
        && cached_application->transport_pair_application_ready_cached ()
        && cached_application->get_peer_socket_type ()
             == target_.source_peer_socket_type
        && cached_application->get_transport_pair_id ()
             == target_.transport_pair_id
        && cached_application->get_transport_pair_generation ()
             == target_.transport_pair_generation
        && cached_application->get_transport_connection_id () != 0
        && cached_application->is_lifecycle_active ()) {
            zlink::pipe_t *const cached =
              lane == transport_lane_application
                ? (cached_application->retain_lifetime_ref ()
                     ? cached_application
                     : NULL)
                : cached_application
                    ->retain_transport_pair_completion_pipe ();
            if (cached
                && cached->get_transport_lane () == lane
                && cached->get_peer_socket_type ()
                     == target_.source_peer_socket_type
                && cached->get_transport_connection_id () != 0
                && cached->is_lifecycle_active ()) {
                errno = 0;
                return cached;
            }
            if (cached)
                cached->release_lifetime_ref ();
        }

    // The logical reply token survives reconnect, but the physical transport
    // never does. Resolve every FINAL through the socket's current route
    // owner: on ROUTER this also excludes an active duplicate-RID standby,
    // while DEALER selects the latest ready pair for the settled peer.
    zlink::pipe_t *const selected =
      socket_->retain_current_transport_pair_pipe (
        peer_rid_, target_.source_peer_socket_type, lane);
    if (!selected) {
        errno = ENOTCONN;
        return NULL;
    }

    const bool current_route =
      selected->get_transport_lane () == lane
      && selected->get_peer_socket_type () == target_.source_peer_socket_type
      && selected->get_transport_pair_id () != 0
      && selected->get_transport_pair_generation () != 0
      && selected->get_transport_connection_id () != 0
      && selected->is_lifecycle_active ();
    if (!current_route) {
        selected->release_lifetime_ref ();
        errno = ENOTCONN;
        return NULL;
    }

    errno = 0;
    return selected;
}

zlink::pipe_t *retain_reply_completion_pipe (
  zlink::socket_base_t *socket_, zlink::pipe_t *application_pipe_,
  const zlink_routing_id_t *peer_rid_)
{
    if (!socket_) {
        errno = EFAULT;
        return NULL;
    }

    // Both accessors return a pinned pipe. Prefer the physical source that
    // received the REQUEST while it is still current, then resolve the latest
    // ready completion lane for the same logical RID after reconnect.
    zlink::pipe_t *completion =
      application_pipe_
        ? socket_->completion_pipe_for_application (application_pipe_)
        : socket_->completion_pipe_for_peer (peer_rid_);
    if (!completion && application_pipe_ && peer_rid_)
        completion = socket_->completion_pipe_for_peer (peer_rid_);
    if (!completion)
        errno = ENOTCONN;
    return completion;
}

int send_completion_staged_frames_on_pipe (
  zlink::pipe_t *completion_, zlink_msg_t *staged_parts_,
  size_t staged_part_count_, zlink_msg_t *final_part_,
  bool preserve_initial_failure_,
  zlink::pipe_message_admission_t *first_admission_out_)
{
    if (first_admission_out_)
        *first_admission_out_ = zlink::pipe_message_admission_invalid;
    const bool valid_staged_range =
      staged_part_count_ == 0 || staged_parts_ != NULL;
    if (!completion_ || !valid_staged_range || !final_part_
        || !reinterpret_cast<zlink::msg_t *> (final_part_)->check ()) {
        zlink::request_reply::consume_send_frames_from (
          staged_parts_, 0, staged_part_count_);
        zlink::request_reply::consume_send_frame (final_part_);
        if (completion_)
            completion_->release_lifetime_ref ();
        errno = EFAULT;
        return -1;
    }

    int rc = 0;
    const size_t total_part_count = staged_part_count_ + 1;
    {
        // engine_error() clears the connection id under this same generation
        // lock. Hold it from the snapshot through the terminal flush so either
        // the complete reply precedes that error, or no input frame moves and
        // the caller observes retryable EAGAIN.
        zlink::scoped_fast_lock_t transport_generation_lock (
          completion_->transport_sync ());
        const uint64_t transport_connection_id =
          completion_->get_transport_connection_id ();
        if (transport_connection_id == 0
            || !completion_->is_lifecycle_active ()) {
            if (first_admission_out_)
                *first_admission_out_ =
                  zlink::pipe_message_admission_inactive;
            if (!preserve_initial_failure_) {
                zlink::request_reply::consume_send_frames_from (
                  staged_parts_, 0, staged_part_count_);
                zlink::request_reply::consume_send_frame (final_part_);
            }
            errno = EAGAIN;
            rc = -1;
        }

        // Every part in one logical reply belongs to the same transport
        // connection. This helper also accepts a DEALER-facing Application
        // pipe; its ordinary write admission supplies HWM and PAUSED behavior.
        for (size_t i = 0; rc == 0 && i < total_part_count; ++i) {
            zlink_msg_t *part =
              i < staged_part_count_ ? &staged_parts_[i] : final_part_;
            zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part);
            if (i + 1 < total_part_count)
                msg->set_flags (zlink::msg_t::more);
            else
                msg->reset_flags (zlink::msg_t::more);
            msg->set_transport_connection_id (transport_connection_id);
            bool written = false;
            zlink::pipe_message_admission_t admission =
              zlink::pipe_message_admission_invalid;
#ifdef ZLINK_BUILD_TESTS
            const bool force_failure =
              i != 0 && test_take_request_reply_write_failure_after_prefix ();
            if (!force_failure)
#endif
                written = i + 1 < total_part_count
                            ? completion_->write (
                                msg, i == 0 ? &admission : NULL)
                            : completion_->write_and_flush (
                                msg, i == 0 ? &admission : NULL);
            if (i == 0 && first_admission_out_)
                *first_admission_out_ = admission;
            if (!written) {
                const int write_errno = errno;
                completion_->rollback ();
                if (!(preserve_initial_failure_ && i == 0)) {
                    if (i < staged_part_count_) {
                        zlink::request_reply::consume_send_frames_from (
                          staged_parts_, i, staged_part_count_);
                        zlink::request_reply::consume_send_frame (final_part_);
                    } else
                        zlink::request_reply::consume_send_frame (final_part_);
                }
                // Before the first frame moves, a disappearing completion
                // route is retryable within the caller's SNDTIMEO budget.
                // Once a prefix moved, rollback prevents wire admission but
                // its ownership is no longer reconstructable here; surface
                // the exact runtime failure and let the application retry its
                // retained complete reply.
                if (preserve_initial_failure_ && i == 0) {
                    switch (admission) {
                        case zlink::pipe_message_admission_hwm_full:
                        case zlink::pipe_message_admission_transport_wait:
                        case zlink::pipe_message_admission_inactive:
                            errno = EAGAIN;
                            break;
                        case zlink::pipe_message_admission_too_large:
                            errno = EMSGSIZE;
                            break;
                        default:
                            errno = write_errno != 0 ? write_errno : EIO;
                            break;
                    }
                } else
                    errno = EIO;
                rc = -1;
                break;
            }
#ifdef ZLINK_BUILD_TESTS
            if (i == 0 && total_part_count > 1)
                test_invoke_request_reply_write_after_prefix_hook ();
#endif
            const int init_rc = zlink_msg_init (part);
            errno_assert (init_rc == 0);
        }
    }
    if (rc == 0)
        errno = 0;
    //  Dropping the pin can run the pipe destructor, so preserve the errno
    //  this function reports across it.
    const int reported_errno = errno;
    completion_->release_lifetime_ref ();
    errno = reported_errno;
    return rc;
}

int send_completion_staged_frames (zlink::socket_base_t *socket_,
                                   zlink::pipe_t *application_pipe_,
                                   const zlink_routing_id_t *peer_rid_,
                                   zlink_msg_t *staged_parts_,
                                   size_t staged_part_count_,
                                   zlink_msg_t *final_part_)
{
    zlink::pipe_t *const completion = retain_reply_completion_pipe (
      socket_, application_pipe_, peer_rid_);
    if (!completion) {
        const int saved_errno = errno;
        zlink::request_reply::consume_send_frames_from (
          staged_parts_, 0, staged_part_count_);
        zlink::request_reply::consume_send_frame (final_part_);
        errno = saved_errno;
        return -1;
    }
    return send_completion_staged_frames_on_pipe (
      completion, staged_parts_, staged_part_count_, final_part_, false);
}

}
}

bool zlink::socket_base_t::router_reply_receive_slot_available () const
{
    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      request_reply_state ();
    if (!state)
        return true;

    std::lock_guard<std::mutex> lock (state->mutex);
    return !state->closing
           && state->reply_target_slots
                < zlink::socket_reqrep_internal::max_reply_target_slots;
}

void zlink::socket_base_t::reply_target_slots_released (
  size_t released_slots_)
{
    if (released_slots_ == 0)
        return;

    const int saved_errno = errno;
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t lock (receive.sync);
    const size_t redriven =
      xredrive_reply_token_waiters (released_slots_);
    if (redriven != 0)
        notify_receive_progress_locked ();
    errno = saved_errno;
}

void zlink::socket_base_t::revoke_router_reply_targets_for_rid (
  const zlink_routing_id_t *peer_rid_)
{
    zlink::socket_reqrep_internal::revoke_router_reply_targets_for_rid (
      request_reply_state (), peer_rid_);
}
