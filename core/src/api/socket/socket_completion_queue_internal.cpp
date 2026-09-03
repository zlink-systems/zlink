/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <chrono>
#include <cstring>
#include <new>

#include "api/socket/socket_completion_queue_internal.hpp"
#include "utils/allocator.hpp"
#include "utils/err.hpp"

namespace
{
void close_completion_payload (zlink_completion_t *completion_)
{
    if (!completion_ || !completion_->reply_parts)
        return;
    zlink_multipart_close (completion_->reply_parts,
                           completion_->reply_part_count);
    zlink::dealloc (completion_->reply_parts);
    completion_->reply_parts = NULL;
    completion_->reply_part_count = 0;
}

void unlink_live_locked (zlink::socket_completion::queue_state_t *state_,
                         zlink::socket_completion::reservation_t *node_)
{
    if (node_->all_previous)
        node_->all_previous->all_next = node_->all_next;
    else
        state_->all_head = node_->all_next;
    if (node_->all_next)
        node_->all_next->all_previous = node_->all_previous;
    node_->all_previous = NULL;
    node_->all_next = NULL;
    zlink_assert (state_->outstanding > 0);
    --state_->outstanding;
}

void destroy_live_chain (zlink::socket_completion::reservation_t *head_)
{
    while (head_) {
        zlink::socket_completion::reservation_t *next = head_->all_next;
        close_completion_payload (&head_->completion);
        if (head_->heap_owned)
            delete head_;
        head_ = next;
    }
}

void destroy_cached_chain (zlink::socket_completion::reservation_t *head_)
{
    while (head_) {
        zlink::socket_completion::reservation_t *next = head_->ready_next;
        if (head_->heap_owned)
            delete head_;
        head_ = next;
    }
}

void reset_reservation (
  zlink::socket_completion::reservation_t *node_)
{
    memset (&node_->completion, 0, sizeof (node_->completion));
    node_->completion.struct_size = sizeof (node_->completion);
    node_->ready_next = NULL;
    node_->all_previous = NULL;
    node_->all_next = NULL;
    node_->ready = false;
}

zlink::socket_completion::reservation_t *cache_released_locked (
  zlink::socket_completion::queue_state_t *state_,
  zlink::socket_completion::reservation_t *node_)
{
    reset_reservation (node_);
    if (state_->cached >= zlink::socket_completion::max_cached_reservations)
        return node_;
    node_->ready_next = state_->cached_head;
    state_->cached_head = node_;
    ++state_->cached;
    return NULL;
}
}

zlink::socket_completion::reservation_t::reservation_t () :
    ready_next (NULL),
    all_previous (NULL),
    all_next (NULL),
    ready (false),
    heap_owned (true)
{
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
}

zlink::socket_completion::queue_state_t::queue_state_t () :
    ready_head (NULL),
    ready_tail (NULL),
    all_head (NULL),
    cached_head (NULL),
    outstanding (0),
    cached (0),
    next_id (1),
    lifecycle_errno (0)
{
    for (size_t i = 0; i != inline_cached_reservations; ++i) {
        inline_cache[i].heap_owned = false;
        inline_cache[i].ready_next = cached_head;
        cached_head = &inline_cache[i];
        ++cached;
    }
}

zlink::socket_completion::queue_state_t::~queue_state_t ()
{
    destroy_live_chain (all_head);
    destroy_cached_chain (cached_head);
}

int zlink::socket_completion::reserve (
  queue_state_t *state_, zlink_completion_kind_t kind_, void *user_context_,
  const zlink_routing_id_t *peer_rid_, reservation_t **reservation_out_,
  zlink_completion_id_t *completion_id_out_)
{
    if (!state_ || !reservation_out_ || !completion_id_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;
    *completion_id_out_ = 0;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->lifecycle_errno != 0) {
        errno = state_->lifecycle_errno;
        return -1;
    }
    if (state_->outstanding >= max_outstanding_completions) {
        errno = EAGAIN;
        return -1;
    }
    if (state_->next_id == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    // Capacity and sequence exhaustion are contract outcomes, independent of
    // allocator state. Allocate only after those checks while retaining the
    // same mutex so the accepted reservation cannot race the 65,536 bound.
    reservation_t *node = state_->cached_head;
    if (node) {
        state_->cached_head = node->ready_next;
        --state_->cached;
        reset_reservation (node);
    } else {
        node = new (std::nothrow) reservation_t ();
        if (!node) {
            errno = ENOMEM;
            return -1;
        }
    }

    node->completion.kind = kind_;
    node->completion.completion_id = state_->next_id++;
    node->completion.user_context = user_context_;
    if (peer_rid_)
        node->completion.peer_rid = *peer_rid_;
    node->all_next = state_->all_head;
    if (state_->all_head)
        state_->all_head->all_previous = node;
    state_->all_head = node;
    ++state_->outstanding;

    *reservation_out_ = node;
    *completion_id_out_ = node->completion.completion_id;
    errno = 0;
    return 0;
}

void zlink::socket_completion::release (queue_state_t *state_,
                                        reservation_t *reservation_)
{
    if (!state_ || !reservation_)
        return;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        if (reservation_->ready)
            return;
        unlink_live_locked (state_, reservation_);
    }

    // Closing a message may invoke an application-owned free callback. Keep
    // that callback outside the queue mutex so it can safely re-enter public
    // completion APIs. The socket operation still owns the detached node.
    close_completion_payload (&reservation_->completion);
    reservation_t *released = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        released = cache_released_locked (state_, reservation_);
    }
    delete released;
}

namespace
{
int publish_locked (zlink::socket_completion::queue_state_t *state_,
                    zlink::socket_completion::reservation_t *reservation_)
{
    if (!state_ || !reservation_) {
        errno = EFAULT;
        return -1;
    }
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->lifecycle_errno != 0) {
        errno = state_->lifecycle_errno;
        return -1;
    }
    if (reservation_->ready) {
        errno = EALREADY;
        return -1;
    }
    reservation_->ready = true;
    reservation_->ready_next = NULL;
    if (state_->ready_tail)
        state_->ready_tail->ready_next = reservation_;
    else
        state_->ready_head = reservation_;
    state_->ready_tail = reservation_;
    state_->changed.notify_all ();
    errno = 0;
    return 0;
}
}

int zlink::socket_completion::publish_send (
  queue_state_t *state_, reservation_t *reservation_,
  zlink_send_complete_result_t result_, int terminal_errno_)
{
    if (!reservation_) {
        errno = EFAULT;
        return -1;
    }
    reservation_->completion.send_result = result_;
    reservation_->completion.send_terminal_errno = terminal_errno_;
    return publish_locked (state_, reservation_);
}

int zlink::socket_completion::publish_request (
  queue_state_t *state_, reservation_t *reservation_,
  zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_)
{
    if (!reservation_ || (!parts_ && part_count_ != 0)) {
        errno = EFAULT;
        return -1;
    }
    reservation_->completion.request_result = result_;
    reservation_->completion.reply_parts = parts_;
    reservation_->completion.reply_part_count = part_count_;
    return publish_locked (state_, reservation_);
}

int zlink::socket_completion::recv (queue_state_t *state_,
                                    zlink_completion_t *completion_out_,
                                    zlink_recv_flags_t flags_, int timeout_ms_)
{
    if (!state_ || !completion_out_) {
        errno = EFAULT;
        return -1;
    }

    std::unique_lock<std::mutex> lock (state_->mutex);
    if (!state_->ready_head && state_->lifecycle_errno == 0
        && flags_ != ZLINK_RECV_FLAGS_DONTWAIT && timeout_ms_ != 0) {
        if (timeout_ms_ < 0) {
            state_->changed.wait (lock, [state_] {
                return state_->ready_head != NULL
                       || state_->lifecycle_errno != 0;
            });
        } else {
            state_->changed.wait_for (
              lock, std::chrono::milliseconds (timeout_ms_), [state_] {
                  return state_->ready_head != NULL
                         || state_->lifecycle_errno != 0;
              });
        }
    }

    if (!state_->ready_head) {
        errno = state_->lifecycle_errno != 0 ? state_->lifecycle_errno : EAGAIN;
        return -1;
    }

    reservation_t *node = state_->ready_head;
    state_->ready_head = node->ready_next;
    if (!state_->ready_head)
        state_->ready_tail = NULL;
    node->ready_next = NULL;
    unlink_live_locked (state_, node);

    *completion_out_ = node->completion;
    node->completion.reply_parts = NULL;
    node->completion.reply_part_count = 0;
    reservation_t *released = cache_released_locked (state_, node);
    lock.unlock ();
    delete released;
    errno = 0;
    return 0;
}

bool zlink::socket_completion::has_ready (queue_state_t *state_)
{
    if (!state_)
        return false;
    std::lock_guard<std::mutex> lock (state_->mutex);
    return state_->ready_head != NULL;
}

size_t zlink::socket_completion::outstanding (queue_state_t *state_)
{
    if (!state_)
        return 0;
    std::lock_guard<std::mutex> lock (state_->mutex);
    return state_->outstanding;
}

void zlink::socket_completion::close (queue_state_t *state_,
                                      int lifecycle_errno_)
{
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->lifecycle_errno == 0)
        state_->lifecycle_errno = lifecycle_errno_;
    // Close drops public delivery, but keeps reservation nodes alive until
    // their in-flight resolver releases them or the socket is destroyed.
    // This avoids freeing a node held by an admission attempt that lost the
    // close race after the lifecycle gate was sealed.
    state_->ready_head = NULL;
    state_->ready_tail = NULL;
    state_->changed.notify_all ();
}
