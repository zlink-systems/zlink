/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

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

dealer_reply_target_t::dealer_reply_target_t () : pipe (NULL), request_seq (0)
{
}

socket_request_reply_state_t::socket_request_reply_state_t (zlink::socket_base_t *socket_,
                                                            int socket_type_) :
    socket (socket_),
    socket_type (socket_type_),
    reply_target_slots (0),
    dealer_next_reply_token (1),
    closing (false),
    completion_control_handler (NULL),
    completion_control_userdata (NULL)
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
    pending_key_t key;
};

void on_socket_request_timeout (void *userdata_)
{
    std::unique_ptr<socket_timeout_callback_ctx_t> ctx (
      static_cast<socket_timeout_callback_ctx_t *> (userdata_));
    if (!ctx.get () || !ctx->state)
        return;

    pending_request_t pending;
    if (remove_socket_pending_request (ctx->state, ctx->key, &pending))
        queue_socket_pending_timeout_completion (ctx->state, pending);
}
}

bool remove_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                    const pending_key_t &key_,
                                    pending_request_t *pending_out_)
{
    if (!state_)
        return false;

    std::lock_guard<std::mutex> lock (state_->mutex);
    return remove_socket_pending_request_locked (state_.get (), key_, pending_out_);
}

int schedule_socket_pending_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  uint32_t timeout_ms_,
  std::shared_ptr<zlink::request_timeout::task_t> *task_out_)
{
    return zlink::request_reply_runtime::schedule_timeout_task<socket_timeout_callback_ctx_t> (
      timeout_ms_, &on_socket_request_timeout,
      [&] (socket_timeout_callback_ctx_t &ctx_) {
          ctx_.state = state_;
          ctx_.key = key_;
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
find_or_create_request_reply_state (socket_handle_t handle_)
{
    std::shared_ptr<socket_request_reply_state_t> state =
      handle_.socket ? handle_.socket->request_reply_state ()
                     : std::shared_ptr<socket_request_reply_state_t> ();
    if (state)
        return state;

    state.reset (new socket_request_reply_state_t (handle_.socket, socket_type (handle_)));
    return handle_.socket->set_request_reply_state (state);
}

std::shared_ptr<socket_request_reply_state_t> find_request_reply_state (socket_handle_t handle_)
{
    return handle_.socket ? handle_.socket->request_reply_state ()
                          : std::shared_ptr<socket_request_reply_state_t> ();
}

}
}
