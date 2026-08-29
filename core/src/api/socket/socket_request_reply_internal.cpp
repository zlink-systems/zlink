/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <atomic>
#include <memory>
#include <new>

#include "api/socket/request_completion_queue_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
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

dealer_reply_target_t::dealer_reply_target_t () :
    pipe (NULL), request_seq (0), checked_out (false)
{
}

router_reply_target_t::router_reply_target_t () : pipe (NULL), checked_out (false)
{
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
    closing (false)
{
}

int ensure_completion_queue_ready (const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_ || !state_->socket) {
        errno = EFAULT;
        return -1;
    }
    errno = 0;
    return 0;
}

int queue_reply_completion (const std::shared_ptr<socket_request_reply_state_t> &state_,
                            zlink_reply_handler_fn handler_,
                            void *userdata_,
                            int errnum_,
                            zlink_msg_t *parts_,
                            size_t part_count_)
{
    if (!state_ || !state_->socket || !state_->socket->get_ctx ()) {
        errno = EFAULT;
        return -1;
    }

    const int rc = zlink::request_completion::enqueue (
      &state_->completion, state_->socket, handler_, userdata_, errnum_, parts_, part_count_);
    return rc;
}

int drain_reply_completions (const std::shared_ptr<socket_request_reply_state_t> &state_,
                             void *owner_handle_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }
    return zlink::request_completion::drain (&state_->completion, owner_handle_);
}

int drain_reply_completions_while_closing (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  void *owner_handle_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }
    return zlink::request_completion::drain_while_closing (
      &state_->completion, owner_handle_);
}

bool has_pending_reply_completions (const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    return state_ ? zlink::request_completion::has_pending (&state_->completion) : false;
}

void claim_completion_owner (const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    if (!state_)
        return;
    zlink::request_completion::claim_owner_thread (&state_->completion);
}

bool current_thread_is_completion_owner (
  const std::shared_ptr<socket_request_reply_state_t> &state_)
{
    return state_ ? zlink::request_completion::current_thread_is_owner (&state_->completion)
                  : false;
}

bool in_socket_request_completion_callback (void *socket_)
{
    return zlink::request_completion::in_request_completion_callback (socket_);
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

    zlink::socket_callback_scope_t callback_scope (ctx->state->socket);
    if (!callback_scope.acquired ())
        return;

    pending_request_t pending;
    if (remove_socket_pending_request (ctx->state, ctx->identity, &pending)) {
#ifdef ZLINK_BUILD_TESTS
        invoke_request_reply_timeout_after_remove_hook ();
#endif
        queue_socket_pending_timeout_completion (ctx->state, pending);
    }
}
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
  const std::shared_ptr<socket_request_reply_state_t> &state_, const pending_request_t &pending_)
{
    (void) queue_reply_completion (state_, pending_.handler, pending_.userdata, ETIMEDOUT, NULL,
                                   0);
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
