/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <atomic>
#include <memory>
#include <new>
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
namespace
{
size_t hash_combine (size_t seed_, size_t value_)
{
    return seed_ ^ (value_ + 0x9e3779b97f4a7c15ULL + (seed_ << 6) + (seed_ >> 2));
}

#ifdef ZLINK_BUILD_TESTS
std::atomic<int> g_request_reply_allocation_failpoint (
  request_reply_allocation_none);
std::atomic<bool> g_request_reply_write_failure_after_prefix (false);
std::mutex g_request_reply_write_prefix_hook_mutex;
request_reply_write_after_prefix_hook_fn
  g_request_reply_write_after_prefix_hook = NULL;
void *g_request_reply_write_after_prefix_userdata = NULL;
std::mutex g_request_reply_timeout_hook_mutex;
request_reply_timeout_after_remove_hook_fn
  g_request_reply_timeout_after_remove_hook = NULL;
void *g_request_reply_timeout_after_remove_userdata = NULL;

void invoke_request_reply_timeout_after_remove_hook ()
{
    request_reply_timeout_after_remove_hook_fn hook = NULL;
    void *userdata = NULL;
    {
        std::lock_guard<std::mutex> lock (
          g_request_reply_timeout_hook_mutex);
        hook = g_request_reply_timeout_after_remove_hook;
        userdata = g_request_reply_timeout_after_remove_userdata;
    }
    if (hook)
        hook (userdata);
}
#endif
}

fixed_routing_id_key_t::fixed_routing_id_key_t () : _size (0) {}

fixed_routing_id_key_t::fixed_routing_id_key_t (
  const fixed_routing_id_key_t &other_) :
    _size (other_._size)
{
    if (_size != 0)
        memcpy (_data, other_._data, _size);
}

fixed_routing_id_key_t &fixed_routing_id_key_t::operator= (
  const fixed_routing_id_key_t &other_)
{
    if (this == &other_)
        return *this;
    _size = other_._size;
    if (_size != 0)
        memcpy (_data, other_._data, _size);
    return *this;
}

void fixed_routing_id_key_t::assign (const void *data_, size_t size_)
{
    if (!data_ || size_ == 0 || size_ > sizeof (_data)) {
        _size = 0;
        return;
    }
    _size = static_cast<uint8_t> (size_);
    memcpy (_data, data_, size_);
}

void fixed_routing_id_key_t::clear ()
{
    _size = 0;
}

bool fixed_routing_id_key_t::empty () const
{
    return _size == 0;
}

size_t fixed_routing_id_key_t::size () const
{
    return _size;
}

const unsigned char *fixed_routing_id_key_t::data () const
{
    return _data;
}

size_t fixed_routing_id_key_t::hash () const
{
    size_t value = sizeof (size_t) == 8
                     ? static_cast<size_t> (14695981039346656037ULL)
                     : static_cast<size_t> (2166136261U);
    const size_t prime = sizeof (size_t) == 8
                           ? static_cast<size_t> (1099511628211ULL)
                           : static_cast<size_t> (16777619U);
    for (size_t i = 0; i != _size; ++i) {
        value ^= static_cast<size_t> (_data[i]);
        value *= prime;
    }
    return value;
}

bool fixed_routing_id_key_t::operator== (
  const fixed_routing_id_key_t &other_) const
{
    return _size == other_._size
           && (_size == 0 || memcmp (_data, other_._data, _size) == 0);
}

bool fixed_routing_id_key_t::operator< (
  const fixed_routing_id_key_t &other_) const
{
    const size_t common = std::min (size (), other_.size ());
    const int compared = common == 0 ? 0 : memcmp (_data, other_._data, common);
    return compared < 0 || (compared == 0 && _size < other_._size);
}

size_t fixed_routing_id_key_hash_t::operator() (
  const fixed_routing_id_key_t &key_) const
{
    return key_.hash ();
}

request_correlation_lease_t::request_correlation_lease_t () :
    _pipe (NULL), _accounted_bytes (0)
{
}

request_correlation_lease_t::~request_correlation_lease_t ()
{
    release ();
}

request_correlation_lease_t::request_correlation_lease_t (
  request_correlation_lease_t &&other_) noexcept :
    _pipe (other_._pipe), _accounted_bytes (other_._accounted_bytes)
{
    other_._pipe = NULL;
    other_._accounted_bytes = 0;
}

request_correlation_lease_t &request_correlation_lease_t::operator= (
  request_correlation_lease_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    release ();
    _pipe = other_._pipe;
    _accounted_bytes = other_._accounted_bytes;
    other_._pipe = NULL;
    other_._accounted_bytes = 0;
    return *this;
}

void request_correlation_lease_t::adopt (zlink::pipe_t *pipe_,
                                         uint64_t accounted_bytes_)
{
    zlink_assert (!_pipe);
    zlink_assert (pipe_);
    zlink_assert (accounted_bytes_ != 0);
    _pipe = pipe_;
    _accounted_bytes = accounted_bytes_;
}

void request_correlation_lease_t::release ()
{
    if (!_pipe)
        return;
    zlink::pipe_t *const pipe = _pipe;
    const uint64_t accounted_bytes = _accounted_bytes;
    _pipe = NULL;
    _accounted_bytes = 0;
    pipe->release_request_correlation (accounted_bytes);
    pipe->release_lifetime_ref ();
}

zlink::pipe_t *request_correlation_lease_t::pipe () const
{
    return _pipe;
}

uint64_t request_correlation_lease_t::accounted_bytes () const
{
    return _accounted_bytes;
}

pending_request_t::pending_request_t () :
    transport_pair_id (0),
    transport_pair_generation (0),
    resolved_timeout_ms (0),
    pull_completion (NULL),
    timeout_deadline_ns (0)
{
}

pending_request_store_t::node_t::node_t () :
    first (0),
    bucket_next (NULL),
    live_previous (NULL),
    live_next (NULL),
    free_next (NULL)
{
}

pending_request_store_t::pending_request_store_t () :
    _free_head (NULL),
    _live_head (NULL),
    _live_tail (NULL),
    _slabs (NULL),
    _size (0),
    _capacity (inline_node_count)
{
    memset (_buckets, 0, sizeof (_buckets));
    add_free_nodes (_inline_nodes, inline_node_count);
}

pending_request_store_t::~pending_request_store_t ()
{
    while (_slabs) {
        slab_t *const next = _slabs->next;
        delete _slabs;
        _slabs = next;
    }
}

size_t pending_request_store_t::bucket_for (uint64_t request_seq_)
{
    request_seq_ ^= request_seq_ >> 33;
    request_seq_ *= UINT64_C (0xff51afd7ed558ccd);
    request_seq_ ^= request_seq_ >> 33;
    request_seq_ *= UINT64_C (0xc4ceb9fe1a85ec53);
    request_seq_ ^= request_seq_ >> 33;
    return static_cast<size_t> (request_seq_)
           & static_cast<size_t> (bucket_count - 1);
}

void pending_request_store_t::add_free_nodes (node_t *nodes_, size_t count_)
{
    for (size_t i = 0; i != count_; ++i) {
        nodes_[i].free_next = _free_head;
        _free_head = &nodes_[i];
    }
}

bool pending_request_store_t::grow ()
{
    if (_capacity >= max_reply_target_slots) {
        errno = EAGAIN;
        return false;
    }
    slab_t *const slab = new (std::nothrow) slab_t ();
    if (!slab) {
        errno = ENOMEM;
        return false;
    }
    slab->next = _slabs;
    _slabs = slab;
    add_free_nodes (slab->nodes, slab_node_count);
    _capacity += slab_node_count;
    return true;
}

std::pair<pending_request_store_t::iterator, bool>
pending_request_store_t::emplace (uint64_t request_seq_,
                                  pending_request_t &&pending_)
{
    if (request_seq_ == 0 || find (request_seq_) != end ()) {
        errno = EEXIST;
        return std::make_pair (end (), false);
    }
    if (!_free_head && !grow ())
        return std::make_pair (end (), false);

    node_t *const node = _free_head;
    _free_head = node->free_next;
    node->free_next = NULL;
    node->first = request_seq_;
    node->second = std::move (pending_);

    const size_t bucket = bucket_for (request_seq_);
    node->bucket_next = _buckets[bucket];
    _buckets[bucket] = node;
    node->live_previous = _live_tail;
    node->live_next = NULL;
    if (_live_tail)
        _live_tail->live_next = node;
    else
        _live_head = node;
    _live_tail = node;
    ++_size;
    errno = 0;
    return std::make_pair (iterator (node), true);
}

pending_request_store_t::iterator
pending_request_store_t::find (uint64_t request_seq_)
{
    if (request_seq_ == 0)
        return end ();
    node_t *node = _buckets[bucket_for (request_seq_)];
    while (node && node->first != request_seq_)
        node = node->bucket_next;
    return iterator (node);
}

pending_request_store_t::const_iterator
pending_request_store_t::find (uint64_t request_seq_) const
{
    if (request_seq_ == 0)
        return end ();
    const node_t *node = _buckets[bucket_for (request_seq_)];
    while (node && node->first != request_seq_)
        node = node->bucket_next;
    return const_iterator (node);
}

size_t pending_request_store_t::count (uint64_t request_seq_) const
{
    return find (request_seq_) == end () ? 0 : 1;
}

pending_request_store_t::iterator
pending_request_store_t::erase (iterator position_)
{
    node_t *const node = position_._node;
    if (!node)
        return end ();
    node_t *const next = node->live_next;

    const size_t bucket = bucket_for (node->first);
    node_t **link = &_buckets[bucket];
    while (*link && *link != node)
        link = &(*link)->bucket_next;
    if (*link == node)
        *link = node->bucket_next;

    if (node->live_previous)
        node->live_previous->live_next = node->live_next;
    else
        _live_head = node->live_next;
    if (node->live_next)
        node->live_next->live_previous = node->live_previous;
    else
        _live_tail = node->live_previous;

    node->first = 0;
    node->second = pending_request_t ();
    node->bucket_next = NULL;
    node->live_previous = NULL;
    node->live_next = NULL;
    node->free_next = _free_head;
    _free_head = node;
    zlink_assert (_size != 0);
    --_size;
    return iterator (next);
}

dealer_reply_target_t::dealer_reply_target_t () :
    pipe (NULL), request_seq (0), checked_out (false)
{
}

router_reply_target_t::router_reply_target_t () :
    pipe (NULL),
    source_pipe_identity (NULL),
    source_peer_socket_type (0),
    wire_request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    route_binding_token (0),
    checked_out (false),
    revoked (false)
{
}

router_reply_alias_key_t::router_reply_alias_key_t () :
    pipe (NULL),
    source_peer_socket_type (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    wire_request_seq (0)
{
}

bool router_reply_alias_key_t::operator== (
  const router_reply_alias_key_t &other_) const
{
    return pipe == other_.pipe
           && source_peer_socket_type == other_.source_peer_socket_type
           && transport_pair_id == other_.transport_pair_id
           && transport_pair_generation == other_.transport_pair_generation
           && wire_request_seq == other_.wire_request_seq;
}

size_t router_reply_alias_key_hash_t::operator() (
  const router_reply_alias_key_t &key_) const
{
    size_t seed = std::hash<zlink::pipe_t *> () (key_.pipe);
    seed = hash_combine (
      seed, std::hash<int> () (key_.source_peer_socket_type));
    seed = hash_combine (
      seed, std::hash<uint64_t> () (key_.transport_pair_id));
    seed = hash_combine (
      seed, std::hash<uint64_t> () (key_.transport_pair_generation));
    return hash_combine (
      seed, std::hash<uint64_t> () (key_.wire_request_seq));
}

socket_request_reply_state_t::socket_request_reply_state_t (zlink::socket_base_t *socket_,
                                                            int socket_type_) :
    socket (socket_),
    socket_type (socket_type_),
    next_pending_cookie (1),
    pending_timeout_deadline_ns (0),
    pending_timeout_generation (0),
    pending_timeout_dispatching (false),
    reply_target_slots (0),
    reply_target_reservations (0),
    reply_target_checkouts (0),
    dealer_next_reply_token (1),
    router_next_reply_token (1),
    public_router_reply_checkout_token (0),
    public_router_reply_active (false),
    public_router_reply_token (0),
    closing (false)
{
    memset (router_reply_alias_buckets, 0,
            sizeof (router_reply_alias_buckets));
}

void release_pending_request_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_)
{
    if (!state_ || !pending_)
        return;

    zlink::socket_completion::reservation_t *const reservation =
      pending_->pull_completion;
    pending_->pull_completion = NULL;
    if (reservation)
        zlink::socket_completion::release (
          &state_->socket->completion_runtime (), reservation);
}

int publish_pending_request_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_, zlink_request_result_t result_,
  zlink_msg_t *parts_, size_t part_count_)
{
    if (!state_ || !state_->socket || !pending_
        || !pending_->pull_completion) {
        errno = EFAULT;
        return -1;
    }

    zlink::socket_completion::reservation_t *const reservation =
      pending_->pull_completion;
    pending_->pull_completion = NULL;
    const int rc = zlink::socket_completion::publish_request (
      &state_->socket->completion_runtime (), reservation, result_, parts_,
      part_count_);
    if (rc != 0) {
        const int saved_errno = errno;
        zlink::socket_completion::release (
          &state_->socket->completion_runtime (), reservation);
        errno = saved_errno;
        return -1;
    }

    state_->socket->notify_request_completion ();
    errno = 0;
    return 0;
}

namespace
{
struct socket_timeout_callback_ctx_t
{
    std::shared_ptr<socket_request_reply_state_t> state;
    pending_request_identity_t identity;
};

struct socket_aggregate_timeout_callback_ctx_t
{
    std::shared_ptr<socket_request_reply_state_t> state;
    uint64_t generation;
};

uint64_t next_pending_timeout_generation (
  const socket_request_reply_state_t &state_)
{
    const uint64_t next = state_.pending_timeout_generation + 1;
    return next == 0 ? 1 : next;
}

uint32_t remaining_timeout_ms (uint64_t deadline_ns_)
{
    const uint64_t now_ns = zlink::request_timeout::monotonic_now_ns ();
    if (deadline_ns_ <= now_ns)
        return 1;
    const uint64_t remaining_ns = deadline_ns_ - now_ns;
    const uint64_t remaining_ms = (remaining_ns + 999999) / 1000000;
    return remaining_ms > UINT32_MAX ? UINT32_MAX
                                     : static_cast<uint32_t> (remaining_ms);
}

void on_socket_aggregate_request_timeout (void *userdata_);

int schedule_socket_aggregate_timeout_locked (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t deadline_ns_)
{
    const uint64_t generation = next_pending_timeout_generation (*state_);
    std::shared_ptr<zlink::request_timeout::task_t> task;
    const int rc = zlink::request_reply_runtime::schedule_timeout_task<
      socket_aggregate_timeout_callback_ctx_t> (
      remaining_timeout_ms (deadline_ns_),
      &on_socket_aggregate_request_timeout,
      [&] (socket_aggregate_timeout_callback_ctx_t &ctx_) {
          ctx_.state = state_;
          ctx_.generation = generation;
      },
      &task);
    if (rc != 0)
        return -1;

    state_->pending_timeout_task = task;
    state_->pending_timeout_deadline_ns = deadline_ns_;
    state_->pending_timeout_generation = generation;
    return 0;
}

void on_socket_request_timeout (void *userdata_)
{
    std::unique_ptr<socket_timeout_callback_ctx_t> ctx (
      static_cast<socket_timeout_callback_ctx_t *> (userdata_));
    if (!ctx.get () || !ctx->state)
        return;

    pending_request_t pending;
    if (remove_socket_pending_request (ctx->state, ctx->identity, &pending)) {
        release_socket_pending_request_correlation (&pending);
#ifdef ZLINK_BUILD_TESTS
        invoke_request_reply_timeout_after_remove_hook ();
#endif
        queue_socket_pending_timeout_completion (ctx->state, &pending);
    }
}

enum aggregate_timeout_resolution_t
{
    aggregate_timeout_timed_out,
    aggregate_timeout_internal_error
};

bool resolve_aggregate_timeout_request (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_, aggregate_timeout_resolution_t resolution_,
  uint64_t generation_)
{
    release_socket_pending_request_correlation (pending_);
    if (resolution_ == aggregate_timeout_timed_out) {
#ifdef ZLINK_BUILD_TESTS
        invoke_request_reply_timeout_after_remove_hook ();
#endif
    }

    // Close cannot free the socket/completion runtime while this mutex is
    // held: it publishes `closing` through the same mutex before cleanup. If
    // close won first, its completion queue owns and eventually destroys the
    // still-live reservation node, so never dereference the detached pointer.
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->closing
        || generation_ != state_->pending_timeout_generation) {
        pending_->pull_completion = NULL;
        state_->pending_timeout_dispatching = false;
        return false;
    }

    if (resolution_ == aggregate_timeout_timed_out)
        queue_socket_pending_timeout_completion (state_, pending_);
    else
        (void) publish_pending_request_completion (
          state_, pending_, ZLINK_REQUEST_INTERNAL_ERROR, NULL, 0);
    return true;
}

// Allocation failure while preparing the ordinary batch, or failure to
// allocate its successor timer, must not strand an admitted request. Retain
// the original allocation-free one-at-a-time path for those exceptional
// cases. Scheduler failure resolves one armed request with INTERNAL_ERROR and
// then retries scheduling the remaining aggregate.
void dispatch_aggregate_timeouts_one_by_one (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t generation_)
{
    while (true) {
        pending_request_t pending;
        bool have_pending = false;
        aggregate_timeout_resolution_t resolution =
          aggregate_timeout_timed_out;
        {
            std::lock_guard<std::mutex> lock (state_->mutex);
            if (state_->closing
                || generation_ != state_->pending_timeout_generation) {
                state_->pending_timeout_dispatching = false;
                return;
            }

            const uint64_t now_ns = zlink::request_timeout::monotonic_now_ns ();
            uint64_t next_deadline_ns = 0;
            pending_request_store_t::iterator due =
              state_->pending_requests.end ();
            for (pending_request_store_t::iterator it =
                   state_->pending_requests.begin ();
                 it != state_->pending_requests.end (); ++it) {
                const uint64_t deadline_ns = it->second.timeout_deadline_ns;
                if (deadline_ns == 0)
                    continue;
                if (deadline_ns <= now_ns) {
                    due = it;
                    break;
                }
                if (next_deadline_ns == 0 || deadline_ns < next_deadline_ns)
                    next_deadline_ns = deadline_ns;
            }

            if (due != state_->pending_requests.end ()) {
                pending = std::move (due->second);
                state_->pending_requests.erase (due);
                have_pending = true;
            } else if (next_deadline_ns != 0) {
                if (schedule_socket_aggregate_timeout_locked (
                      state_, next_deadline_ns)
                    == 0) {
                    state_->pending_timeout_dispatching = false;
                    return;
                }

                // A physically admitted request cannot lose its terminal
                // completion because the replacement timer allocation failed.
                // Resolve one armed record, then retry scheduling the remaining
                // aggregate; unarmed admission records are left untouched.
                for (pending_request_store_t::iterator
                       it = state_->pending_requests.begin ();
                     it != state_->pending_requests.end (); ++it) {
                    if (it->second.timeout_deadline_ns == 0)
                        continue;
                    pending = std::move (it->second);
                    state_->pending_requests.erase (it);
                    have_pending = true;
                    resolution = aggregate_timeout_internal_error;
                    break;
                }
            } else {
                state_->pending_timeout_dispatching = false;
                return;
            }
        }

        if (!have_pending)
            continue;
        if (!resolve_aggregate_timeout_request (
              state_, &pending, resolution, generation_))
            return;
    }
}

void on_socket_aggregate_request_timeout (void *userdata_)
{
    std::unique_ptr<socket_aggregate_timeout_callback_ctx_t> ctx (
      static_cast<socket_aggregate_timeout_callback_ctx_t *> (userdata_));
    if (!ctx.get () || !ctx->state)
        return;

    const std::shared_ptr<socket_request_reply_state_t> state = ctx->state;
    std::vector<pending_request_t> due;
    bool use_fallback = false;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (ctx->generation != state->pending_timeout_generation)
            return;
        state->pending_timeout_task.reset ();
        state->pending_timeout_deadline_ns = 0;
        state->pending_timeout_dispatching = true;
        if (state->closing) {
            state->pending_timeout_dispatching = false;
            return;
        }

        const uint64_t now_ns = zlink::request_timeout::monotonic_now_ns ();
        uint64_t next_deadline_ns = 0;
        size_t due_count = 0;
        for (pending_request_store_t::const_iterator it =
               state->pending_requests.begin ();
             it != state->pending_requests.end (); ++it) {
            const uint64_t deadline_ns = it->second.timeout_deadline_ns;
            if (deadline_ns == 0)
                continue;
            if (deadline_ns <= now_ns)
                ++due_count;
            else if (next_deadline_ns == 0
                     || deadline_ns < next_deadline_ns)
                next_deadline_ns = deadline_ns;
        }

        // A stale aggregate task commonly fires after its former earliest
        // request already completed. Keep that zero-due path allocation-free.
        if (due_count == 0) {
            if (next_deadline_ns != 0
                && schedule_socket_aggregate_timeout_locked (
                     state, next_deadline_ns)
                     != 0)
                use_fallback = true;
            else
                state->pending_timeout_dispatching = false;
        } else {
            // Reserve before moving any record so allocation failure leaves the
            // aggregate untouched and can safely fall back to the no-allocation
            // path above.
            try {
                due.reserve (due_count);
            } catch (...) {
                use_fallback = true;
            }
        }

        if (due_count != 0 && !use_fallback) {
            for (pending_request_store_t::iterator it =
                   state->pending_requests.begin ();
                 it != state->pending_requests.end ();) {
                const uint64_t deadline_ns = it->second.timeout_deadline_ns;
                if (deadline_ns != 0 && deadline_ns <= now_ns) {
                    due.emplace_back (std::move (it->second));
                    it = state->pending_requests.erase (it);
                    continue;
                }
                ++it;
            }
        }
    }

    bool dispatch_canceled = false;
    for (std::vector<pending_request_t>::iterator it = due.begin ();
         it != due.end (); ++it) {
        if (resolve_aggregate_timeout_request (
              state, &*it, aggregate_timeout_timed_out, ctx->generation))
            continue;

        ++it;
        for (; it != due.end (); ++it) {
            release_socket_pending_request_correlation (&*it);
            it->pull_completion = NULL;
        }
        dispatch_canceled = true;
        break;
    }

    // Keep the callback serialized through completion publication, matching
    // the previous close/cancel lifetime fence. Requests armed meanwhile see
    // pending_timeout_dispatching and leave their deadline in the aggregate;
    // one final scan includes them when selecting the successor task.
    if (!use_fallback && !dispatch_canceled && !due.empty ()) {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (state->closing
            || ctx->generation != state->pending_timeout_generation) {
            state->pending_timeout_dispatching = false;
            return;
        }

        uint64_t next_deadline_ns = 0;
        for (pending_request_store_t::const_iterator it =
               state->pending_requests.begin ();
             it != state->pending_requests.end (); ++it) {
            const uint64_t deadline_ns = it->second.timeout_deadline_ns;
            if (deadline_ns != 0
                && (next_deadline_ns == 0
                    || deadline_ns < next_deadline_ns))
                next_deadline_ns = deadline_ns;
        }
        if (next_deadline_ns != 0
            && schedule_socket_aggregate_timeout_locked (
                 state, next_deadline_ns)
                 != 0)
            use_fallback = true;
        else
            state->pending_timeout_dispatching = false;
    }

    if (use_fallback)
        dispatch_aggregate_timeouts_one_by_one (state, ctx->generation);
}

}

void release_socket_pending_request_correlation (pending_request_t *pending_)
{
    if (pending_)
        pending_->correlation.release ();
}

bool remove_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                    const pending_request_identity_t &identity_,
                                    pending_request_t *pending_out_)
{
    if (!state_)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    return remove_socket_pending_request_locked (state_.get (), identity_, pending_out_);
}

int schedule_socket_pending_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_identity_t &identity_,
  uint32_t timeout_ms_,
  std::shared_ptr<zlink::request_timeout::task_t> *task_out_)
{
    return zlink::request_reply_runtime::schedule_timeout_task<socket_timeout_callback_ctx_t> (
      timeout_ms_, &on_socket_request_timeout,
      [&] (socket_timeout_callback_ctx_t &ctx_) {
          ctx_.state = state_;
          ctx_.identity = identity_;
      },
      task_out_);
}

int arm_socket_pending_request_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_token_t &token_)
{
    if (!state_ || token_.identity.request_seq == 0
        || token_.identity.cookie == 0) {
        errno = EFAULT;
        return -1;
    }

    pending_request_t failed;
    bool removed = false;
    std::shared_ptr<zlink::request_timeout::task_t> replaced_task;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        pending_request_store_t::iterator pending =
          state_->pending_requests.find (token_.identity.request_seq);
        if (pending == state_->pending_requests.end ()
            || !(pending->second.identity == token_.identity)
            || pending->second.timeout_deadline_ns != 0
            || state_->closing) {
            errno = 0;
            return 0;
        }

        const uint64_t deadline_ns = zlink::request_timeout::deadline_after_ms (
          token_.resolved_timeout_ms);
        if (state_->pending_timeout_dispatching
            || (state_->pending_timeout_task
                && state_->pending_timeout_deadline_ns <= deadline_ns)) {
            pending->second.timeout_deadline_ns = deadline_ns;
            errno = 0;
            return 0;
        }

        replaced_task = state_->pending_timeout_task;
        if (schedule_socket_aggregate_timeout_locked (state_, deadline_ns)
            == 0) {
            pending->second.timeout_deadline_ns = deadline_ns;
        } else {
            removed = remove_socket_pending_request_locked (
              state_.get (), token_.identity, &failed);
            replaced_task.reset ();
        }
    }

    if (replaced_task)
        zlink::request_timeout::cancel (replaced_task);
    if (removed) {
        release_socket_pending_request_correlation (&failed);
        (void) publish_pending_request_completion (
          state_, &failed, ZLINK_REQUEST_INTERNAL_ERROR, NULL, 0);
    }
    errno = 0;
    return 0;
}

void cancel_socket_pending_timeouts (
  const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_)
        return;

    std::shared_ptr<zlink::request_timeout::task_t> task;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->pending_timeout_generation =
          next_pending_timeout_generation (*state_);
        state_->pending_timeout_deadline_ns = 0;
        task.swap (state_->pending_timeout_task);
    }
    if (task)
        zlink::request_timeout::cancel (task);
}

void queue_socket_pending_timeout_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_)
{
    if (!pending_)
        return;
    (void) publish_pending_request_completion (
      state_, pending_, ZLINK_REQUEST_TIMED_OUT, NULL, 0);
}

std::shared_ptr<socket_request_reply_state_t>
find_or_create_request_reply_state (const socket_handle_t &handle_)
{
    if (!handle_.socket) {
        errno = EFAULT;
        return std::shared_ptr<socket_request_reply_state_t> ();
    }
    std::shared_ptr<socket_request_reply_state_t> state =
      handle_.socket->request_reply_state ();
    if (state)
        return state;

    try {
        state.reset (new socket_request_reply_state_t (handle_.socket,
                                                       socket_type (handle_)));
    } catch (...) {
        errno = ENOMEM;
        return std::shared_ptr<socket_request_reply_state_t> ();
    }
    return handle_.socket->set_request_reply_state (state);
}

std::shared_ptr<socket_request_reply_state_t>
find_request_reply_state (const socket_handle_t &handle_)
{
    return handle_.socket
             ? handle_.socket->request_reply_state ()
             : std::shared_ptr<socket_request_reply_state_t> ();
}

#ifdef ZLINK_BUILD_TESTS
void test_set_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_)
{
    g_request_reply_allocation_failpoint.store (
      static_cast<int> (failpoint_), std::memory_order_release);
}

void test_throw_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_)
{
    int expected = static_cast<int> (failpoint_);
    if (g_request_reply_allocation_failpoint.compare_exchange_strong (
          expected, static_cast<int> (request_reply_allocation_none),
          std::memory_order_acq_rel, std::memory_order_acquire))
        throw std::bad_alloc ();
}

void test_set_request_reply_write_failure_after_prefix (bool enabled_)
{
    g_request_reply_write_failure_after_prefix.store (
      enabled_, std::memory_order_release);
}

bool test_take_request_reply_write_failure_after_prefix ()
{
    return g_request_reply_write_failure_after_prefix.exchange (
      false, std::memory_order_acq_rel);
}

void test_set_request_reply_write_after_prefix_hook (
  request_reply_write_after_prefix_hook_fn hook_, void *userdata_)
{
    std::lock_guard<std::mutex> lock (
      g_request_reply_write_prefix_hook_mutex);
    g_request_reply_write_after_prefix_userdata = userdata_;
    g_request_reply_write_after_prefix_hook = hook_;
}

void test_invoke_request_reply_write_after_prefix_hook ()
{
    request_reply_write_after_prefix_hook_fn hook = NULL;
    void *userdata = NULL;
    {
        std::lock_guard<std::mutex> lock (
          g_request_reply_write_prefix_hook_mutex);
        hook = g_request_reply_write_after_prefix_hook;
        userdata = g_request_reply_write_after_prefix_userdata;
    }
    if (hook)
        hook (userdata);
}

void test_set_request_reply_timeout_after_remove_hook (
  request_reply_timeout_after_remove_hook_fn hook_, void *userdata_)
{
    std::lock_guard<std::mutex> lock (
      g_request_reply_timeout_hook_mutex);
    g_request_reply_timeout_after_remove_userdata = userdata_;
    g_request_reply_timeout_after_remove_hook = hook_;
}
#endif

}
}
