/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <new>

#include "api/socket/request_completion_queue_internal.hpp"

#include "api/message/request_result_internal.hpp"
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
        dispatch_scope (static_cast<zlink::socket_base_t *> (owner_handle_)),
        previous_owner (request_completion_owner_tls ())
    {
        request_completion_owner_tls () = owner_handle_;
    }

    ~request_completion_callback_scope_t () { request_completion_owner_tls () = previous_owner; }

  private:
    zlink::socket_send_complete_dispatch_scope_t dispatch_scope;
    void *previous_owner;
};

void delete_control_chain (zlink::request_completion::control_t *head_)
{
    while (head_) {
        zlink::request_completion::control_t *next = head_->next;
        delete head_;
        head_ = next;
    }
}

zlink::request_completion::control_t *cache_released_control_locked (
  zlink::request_completion::queue_state_t *state_,
  zlink::request_completion::control_t *node_)
{
    node_->handler = NULL;
    node_->userdata = NULL;
    node_->errnum = 0;
    if (state_->cached < zlink::request_completion::max_cached_controls) {
        node_->next = state_->cached_head;
        state_->cached_head = node_;
        ++state_->cached;
        return NULL;
    }

    node_->next = NULL;
    return node_;
}

void release_ready_node (zlink::request_completion::queue_state_t *state_,
                         zlink::request_completion::control_t *node_)
{
    zlink::request_completion::control_t *released = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        zlink_assert (state_->reserved > 0);
        --state_->reserved;
        released = cache_released_control_locked (state_, node_);
    }
    delete released;
}
}

zlink::request_completion::queue_state_t::queue_state_t () :
    cached_head (NULL),
    cached (0),
    reserved_head (NULL),
    ready_head (NULL),
    ready_tail (NULL),
    reserved (0),
    owner_thread_valid (false),
    closed (false)
{
}

zlink::request_completion::queue_state_t::~queue_state_t ()
{
    delete_control_chain (cached_head);
    delete_control_chain (reserved_head);
    delete_control_chain (ready_head);
}

bool zlink::request_completion::try_reserve (queue_state_t *state_)
{
    if (!state_) {
        errno = EFAULT;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->closed || state_->reserved >= max_pending_completions) {
            const int failure_errno = state_->closed ? ETERM : EAGAIN;
            errno = failure_errno;
            return false;
        }
        if (state_->cached_head) {
            control_t *node = state_->cached_head;
            state_->cached_head = node->next;
            --state_->cached;
            node->next = state_->reserved_head;
            state_->reserved_head = node;
            ++state_->reserved;
            return true;
        }
    }

    control_t *node = new (std::nothrow) control_t ();
    if (!node) {
        errno = ENOMEM;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->closed || state_->reserved >= max_pending_completions) {
            const int failure_errno = state_->closed ? ETERM : EAGAIN;
            delete node;
            errno = failure_errno;
            return false;
        }
        node->next = state_->reserved_head;
        state_->reserved_head = node;
        ++state_->reserved;
    }
    return true;
}

void zlink::request_completion::release_reservation (queue_state_t *state_)
{
    if (!state_)
        return;
    control_t *released = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (state_->reserved > 0) {
            //  Reservations are fungible. A request that completes directly
            //  always has one node in this unqueued pool; terminal controls
            //  move their node to ready_head and release it in drain().
            zlink_assert (state_->reserved_head != NULL);
            released = state_->reserved_head;
            state_->reserved_head = released->next;
            --state_->reserved;
            released = cache_released_control_locked (state_, released);
        } else {
            zlink_assert (state_->closed);
        }
    }
    delete released;
}

zlink::request_completion::control_t::control_t () :
    handler (NULL),
    userdata (NULL),
    errnum (0),
    next (NULL)
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
        if (!state_->reserved_head) {
            errno = EFAULT;
            return -1;
        }
        control_t *control = state_->reserved_head;
        state_->reserved_head = control->next;
        control->handler = handler_;
        control->userdata = userdata_;
        control->errnum = errnum_;
        control->next = NULL;
        if (state_->ready_tail)
            state_->ready_tail->next = control;
        else
            state_->ready_head = control;
        state_->ready_tail = control;
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
    if (handler_)
        handler_ (zlink::request_result_internal::from_errno (errnum_),
                  parts_, part_count_, userdata_);
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

    zlink::request_completion::control_t *controls = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->owner_thread = std::this_thread::get_id ();
        state_->owner_thread_valid = true;
        controls = state_->ready_head;
        state_->ready_head = NULL;
        state_->ready_tail = NULL;
    }

    int drained = 0;
    while (controls) {
        zlink::request_completion::control_t *control = controls;
        controls = control->next;
        control->next = NULL;
        zlink::request_completion::invoke_callback (
          owner_handle_, control->handler, control->errnum, NULL, 0,
          control->userdata);
        release_ready_node (state_, control);
        ++drained;
    }

    errno = 0;
    return drained;
}
}

int zlink::request_completion::drain (queue_state_t *state_, void *owner_handle_)
{
    if (!state_ || !owner_handle_) {
        errno = EFAULT;
        return -1;
    }

    control_t *controls = NULL;
    std::unique_lock<std::mutex> lock (state_->mutex);
    if (!state_->ready_head) {
        errno = 0;
        return 0;
    }

    socket_callback_scope_t callback_scope (
      static_cast<socket_base_t *> (owner_handle_));
    if (!callback_scope.acquired ()) {
        errno = ESHUTDOWN;
        return -1;
    }

    state_->owner_thread = std::this_thread::get_id ();
    state_->owner_thread_valid = true;
    controls = state_->ready_head;
    state_->ready_head = NULL;
    state_->ready_tail = NULL;
    lock.unlock ();

    int drained = 0;
    while (controls) {
        control_t *control = controls;
        controls = control->next;
        control->next = NULL;
        invoke_callback (owner_handle_, control->handler, control->errnum,
                         NULL, 0, control->userdata);
        release_ready_node (state_, control);
        ++drained;
    }

    errno = 0;
    return drained;
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
    control_t *reserved = NULL;
    control_t *ready = NULL;
    control_t *cached = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->closed = true;
        reserved = state_->reserved_head;
        ready = state_->ready_head;
        cached = state_->cached_head;
        state_->reserved_head = NULL;
        state_->ready_head = NULL;
        state_->ready_tail = NULL;
        state_->cached_head = NULL;
        state_->cached = 0;
        state_->reserved = 0;
        state_->owner_thread_valid = false;
    }
    delete_control_chain (reserved);
    delete_control_chain (ready);
    delete_control_chain (cached);
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
    return state_->ready_head != NULL;
}

bool zlink::request_completion::in_request_completion_callback (void *owner_handle_)
{
    return owner_handle_ != NULL && request_completion_owner_tls () == owner_handle_;
}
