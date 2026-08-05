/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/request_completion_queue_internal.hpp"

#include "api/socket/request_reply_protocol_internal.hpp"
#include "sockets/common/socket_base.hpp"

namespace
{
void *&request_completion_owner_tls ()
{
    static thread_local void *owner = NULL;
    return owner;
}

class request_completion_callback_scope_t
{
  public:
    explicit request_completion_callback_scope_t (void *owner_handle_) :
        previous_owner (request_completion_owner_tls ())
    {
        request_completion_owner_tls () = owner_handle_;
    }

    ~request_completion_callback_scope_t () { request_completion_owner_tls () = previous_owner; }

  private:
    void *previous_owner;
};
}

zlink::request_completion::queue_state_t::queue_state_t () :
    reserved (0),
    owner_thread_valid (false),
    closed (false)
{
}

bool zlink::request_completion::try_reserve (queue_state_t *state_)
{
    if (!state_) {
        errno = EFAULT;
        return false;
    }
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->closed || state_->reserved >= max_pending_completions) {
        errno = state_->closed ? ETERM : EAGAIN;
        return false;
    }
    ++state_->reserved;
    return true;
}

void zlink::request_completion::release_reservation (queue_state_t *state_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->reserved > 0)
        --state_->reserved;
    else
        zlink_assert (state_->closed);
}

zlink::request_completion::control_t::control_t () :
    handler (NULL),
    userdata (NULL),
    errnum (0)
{
}

int zlink::request_completion::enqueue (queue_state_t *state_,
                                        void *owner_handle_,
                                        zlink_reply_handler_fn handler_,
                                        void *userdata_,
                                        int errnum_,
                                        zlink_msg_t *parts_,
                                        size_t part_count_)
{
    if (!state_ || !owner_handle_ || !handler_ || parts_ || part_count_ != 0) {
        errno = EFAULT;
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->closed) {
            errno = ETERM;
            return -1;
        }
        control_t control;
        control.handler = handler_;
        control.userdata = userdata_;
        control.errnum = errnum_;
        state_->controls.push_back (control);
    }

    static_cast<zlink::socket_base_t *> (owner_handle_)->notify_request_completion ();
    errno = 0;
    return 0;
}

void zlink::request_completion::invoke_callback (void *owner_handle_,
                                                 zlink_reply_handler_fn handler_,
                                                 int errnum_,
                                                 zlink_msg_t *parts_,
                                                 size_t part_count_,
                                                 void *userdata_)
{
    const request_completion_callback_scope_t scope (owner_handle_);
    zlink::request_reply::complete_reply_callback (
      handler_, errnum_, parts_, part_count_, userdata_);
}

namespace
{
int drain_controls (zlink::request_completion::queue_state_t *state_,
                    void *owner_handle_)
{
    if (!state_ || !owner_handle_) {
        errno = EFAULT;
        return -1;
    }

    std::deque<zlink::request_completion::control_t> controls;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->owner_thread = std::this_thread::get_id ();
        state_->owner_thread_valid = true;
        controls.swap (state_->controls);
    }

    for (std::deque<zlink::request_completion::control_t>::iterator it = controls.begin ();
         it != controls.end (); ++it) {
        zlink::request_completion::invoke_callback (
          owner_handle_, it->handler, it->errnum, NULL, 0, it->userdata);
        zlink::request_completion::release_reservation (state_);
    }

    errno = 0;
    return static_cast<int> (controls.size ());
}
}

int zlink::request_completion::drain (queue_state_t *state_, void *owner_handle_)
{
    socket_callback_scope_t callback_scope (
      static_cast<socket_base_t *> (owner_handle_));
    if (!callback_scope.acquired ()) {
        errno = ESHUTDOWN;
        return -1;
    }
    return drain_controls (state_, owner_handle_);
}

int zlink::request_completion::drain_while_closing (queue_state_t *state_,
                                                     void *owner_handle_)
{
    return drain_controls (state_, owner_handle_);
}

void zlink::request_completion::close (queue_state_t *state_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->closed = true;
    state_->controls.clear ();
    state_->reserved = 0;
    state_->owner_thread_valid = false;
}

void zlink::request_completion::claim_owner_thread (queue_state_t *state_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->owner_thread = std::this_thread::get_id ();
    state_->owner_thread_valid = true;
}

bool zlink::request_completion::current_thread_is_owner (queue_state_t *state_)
{
    if (!state_)
        return false;
    std::lock_guard<std::mutex> lock (state_->mutex);
    return state_->owner_thread_valid && state_->owner_thread == std::this_thread::get_id ();
}

bool zlink::request_completion::has_pending (queue_state_t *state_)
{
    if (!state_)
        return false;
    std::lock_guard<std::mutex> lock (state_->mutex);
    return !state_->controls.empty ();
}

bool zlink::request_completion::in_request_completion_callback (void *owner_handle_)
{
    return owner_handle_ != NULL && request_completion_owner_tls () == owner_handle_;
}
