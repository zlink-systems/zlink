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

bool pending_key_t::operator== (const pending_key_t &other_) const
{
    return request_seq == other_.request_seq && peer_rid == other_.peer_rid;
}

bool pending_key_t::operator< (const pending_key_t &other_) const
{
    if (request_seq != other_.request_seq)
        return request_seq < other_.request_seq;
    return peer_rid < other_.peer_rid;
}

size_t pending_key_hash_t::operator() (const pending_key_t &key_) const
{
    size_t seed = std::hash<uint64_t> () (key_.request_seq);
    return hash_combine (seed, std::hash<std::string> () (key_.peer_rid));
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
    pull_completion (NULL)
{
}

dealer_reply_target_t::dealer_reply_target_t () :
    pipe (NULL), request_seq (0), checked_out (false)
{
}

router_reply_target_t::router_reply_target_t () :
    pipe (NULL),
    source_pipe_identity (NULL),
    wire_request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    checked_out (false)
{
}

router_reply_alias_key_t::router_reply_alias_key_t () :
    pipe (NULL),
    transport_pair_id (0),
    transport_pair_generation (0),
    wire_request_seq (0)
{
}

bool router_reply_alias_key_t::operator== (
  const router_reply_alias_key_t &other_) const
{
    return pipe == other_.pipe && transport_pair_id == other_.transport_pair_id
           && transport_pair_generation == other_.transport_pair_generation
           && wire_request_seq == other_.wire_request_seq;
}

size_t router_reply_alias_key_hash_t::operator() (
  const router_reply_alias_key_t &key_) const
{
    size_t seed = std::hash<zlink::pipe_t *> () (key_.pipe);
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
    reply_target_slots (0),
    reply_target_reservations (0),
    reply_target_checkouts (0),
    dealer_next_reply_token (1),
    router_next_reply_token (1),
    public_router_reply_active (false),
    closing (false)
{
    public_router_reply_key.request_seq = 0;
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
    return handle_.socket && handle_.socket->has_request_reply_state ()
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
