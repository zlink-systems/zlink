/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <chrono>
#include <cstring>
#include <new>

#include "api/socket/socket_completion_queue_internal.hpp"
#include "core/pipe.hpp"
#include "utils/allocator.hpp"
#include "utils/err.hpp"

void zlink::socket_completion::release_payload (
  zlink_completion_t *completion_)
{
    if (!completion_ || !completion_->reply_parts)
        return;
    zlink_multipart_close (completion_->reply_parts,
                           completion_->reply_part_count);
    zlink::dealloc (completion_->reply_parts);
    completion_->reply_parts = NULL;
    completion_->reply_part_count = 0;
}

namespace
{
void unlink_outstanding_locked (
  zlink::socket_completion::queue_state_t *state_,
  zlink::socket_completion::reservation_t *node_)
{
    if (node_->outstanding_previous)
        node_->outstanding_previous->outstanding_next =
          node_->outstanding_next;
    else
        state_->outstanding_head = node_->outstanding_next;
    if (node_->outstanding_next)
        node_->outstanding_next->outstanding_previous =
          node_->outstanding_previous;
    node_->outstanding_previous = NULL;
    node_->outstanding_next = NULL;
    zlink_assert (state_->outstanding > 0);
    --state_->outstanding;
}

void destroy_outstanding_chain (
  zlink::socket_completion::reservation_t *head_)
{
    while (head_) {
        zlink::socket_completion::reservation_t *next =
          head_->outstanding_next;
        zlink::socket_completion::release_payload (&head_->completion);
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
    node_->writable_wait_next = NULL;
    node_->outstanding_previous = NULL;
    node_->outstanding_next = NULL;
    node_->ready = false;
    node_->writable_wait_linked = false;
    node_->request_wait.clear ();
}

zlink::socket_completion::reservation_t *recycle_reservation_locked (
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

int acquire_reservation_locked (
  zlink::socket_completion::queue_state_t *state_,
  zlink_completion_kind_t kind_, void *user_context_,
  const zlink_routing_id_t *peer_rid_,
  zlink::socket_completion::reservation_t **reservation_out_,
  zlink_completion_id_t *completion_id_out_)
{
    if (state_->lifecycle_errno != 0) {
        errno = state_->lifecycle_errno;
        return -1;
    }
    if (state_->outstanding
        >= zlink::socket_completion::max_outstanding_completions) {
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
    zlink::socket_completion::reservation_t *node = state_->cached_head;
    if (node) {
        state_->cached_head = node->ready_next;
        --state_->cached;
    } else {
        node = new (std::nothrow) zlink::socket_completion::reservation_t ();
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
    node->outstanding_next = state_->outstanding_head;
    if (state_->outstanding_head)
        state_->outstanding_head->outstanding_previous = node;
    state_->outstanding_head = node;
    ++state_->outstanding;

    *reservation_out_ = node;
    *completion_id_out_ = node->completion.completion_id;
    errno = 0;
    return 0;
}

void unlink_writable_wait_locked (
  zlink::socket_completion::queue_state_t *state_,
  zlink::socket_completion::reservation_t *node_)
{
    if (!node_->writable_wait_linked)
        return;

    zlink::socket_completion::reservation_t *previous = NULL;
    zlink::socket_completion::reservation_t *current =
      state_->writable_wait_head;
    while (current && current != node_) {
        previous = current;
        current = current->writable_wait_next;
    }
    zlink_assert (current == node_);
    if (previous)
        previous->writable_wait_next = node_->writable_wait_next;
    else
        state_->writable_wait_head = node_->writable_wait_next;
    if (state_->writable_wait_tail == node_)
        state_->writable_wait_tail = previous;
    node_->writable_wait_next = NULL;
    node_->writable_wait_linked = false;
    const size_t previous_count = state_->writable_waiting_count.fetch_sub (
      1, std::memory_order_release);
    zlink_assert (previous_count > 0);
}
}

zlink::socket_completion::reservation_t::reservation_t () :
    ready_next (NULL),
    writable_wait_next (NULL),
    outstanding_previous (NULL),
    outstanding_next (NULL),
    ready (false),
    writable_wait_linked (false),
    heap_owned (true)
{
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
}

zlink::socket_completion::queue_state_t::queue_state_t () :
    ready_head (NULL),
    ready_tail (NULL),
    writable_wait_head (NULL),
    writable_wait_tail (NULL),
    outstanding_head (NULL),
    cached_head (NULL),
    outstanding (0),
    cached (0),
    next_id (1),
    lifecycle_errno (0),
    ready_available (false),
    ready_writable_count (0),
    writable_waiting_count (0)
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
    destroy_outstanding_chain (outstanding_head);
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
    return acquire_reservation_locked (state_, kind_, user_context_, peer_rid_,
                                       reservation_out_, completion_id_out_);
}

int zlink::socket_completion::reserve_writable_wait (
  queue_state_t *state_, void *user_context_,
  const zlink_routing_id_t *peer_rid_, reservation_t **reservation_out_,
  zlink_completion_id_t *completion_id_out_,
  request_writable_wait_t *request_wait_)
{
    if (!state_ || !reservation_out_ || !completion_id_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;
    *completion_id_out_ = 0;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (acquire_reservation_locked (
          state_, ZLINK_COMPLETION_WRITABLE, user_context_, peer_rid_,
          reservation_out_, completion_id_out_)
        != 0) {
        // EAGAIN is reserved by the public SEND contract for a successfully
        // issued nonzero wait token. Exhausting the shared reservation pool is
        // a resource failure and must not escape as BACKPRESSURED with ID 0.
        if (errno == EAGAIN)
            errno = ENOMEM;
        return -1;
    }

    reservation_t *const node = *reservation_out_;
    if (request_wait_)
        node->request_wait.swap (*request_wait_);
    node->writable_wait_next = NULL;
    node->writable_wait_linked = true;
    if (state_->writable_wait_tail)
        state_->writable_wait_tail->writable_wait_next = node;
    else
        state_->writable_wait_head = node;
    state_->writable_wait_tail = node;
    state_->writable_waiting_count.fetch_add (1, std::memory_order_release);
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
        unlink_writable_wait_locked (state_, reservation_);
        unlink_outstanding_locked (state_, reservation_);
    }

    // Closing a message may invoke an application-owned free callback. Keep
    // that callback outside the queue mutex so it can safely re-enter public
    // completion APIs. The socket operation still owns the detached node.
    release_payload (&reservation_->completion);
    reservation_t *node_to_delete = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        node_to_delete = recycle_reservation_locked (state_, reservation_);
    }
    delete node_to_delete;
}

namespace
{
void append_ready_locked (
  zlink::socket_completion::queue_state_t *state_,
  zlink::socket_completion::reservation_t *reservation_)
{
    const bool was_empty = state_->ready_head == NULL;
    reservation_->ready = true;
    reservation_->ready_next = NULL;
    if (state_->ready_tail)
        state_->ready_tail->ready_next = reservation_;
    else
        state_->ready_head = reservation_;
    state_->ready_tail = reservation_;
    if (reservation_->completion.kind == ZLINK_COMPLETION_WRITABLE)
        state_->ready_writable_count.fetch_add (1,
                                                std::memory_order_release);
    if (was_empty)
        state_->ready_available.store (true, std::memory_order_release);
}

bool writable_target_matches (
  const zlink::socket_completion::reservation_t *reservation_,
  const zlink_routing_id_t *target_rid_or_null_)
{
    const zlink_routing_id_t &reserved = reservation_->completion.peer_rid;
    if (!target_rid_or_null_)
        return reserved.size == 0;
    return reserved.size == target_rid_or_null_->size
           && (reserved.size == 0
               || memcmp (reserved.data, target_rid_or_null_->data,
                          reserved.size)
                    == 0);
}

int enqueue_ready (zlink::socket_completion::queue_state_t *state_,
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
    append_ready_locked (state_, reservation_);
    state_->changed.notify_all ();
    errno = 0;
    return 0;
}
}

int zlink::socket_completion::publish_writable_waiters (
  queue_state_t *state_, const zlink_routing_id_t *target_rid_or_null_,
  zlink_send_complete_result_t result_, int terminal_errno_,
  bool correlation_released_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->lifecycle_errno != 0) {
        errno = state_->lifecycle_errno;
        return -1;
    }

    reservation_t *previous = NULL;
    reservation_t *current = state_->writable_wait_head;
    int published = 0;
    while (current) {
        reservation_t *const next = current->writable_wait_next;
        bool matches = writable_target_matches (current, target_rid_or_null_);
        if (result_ != ZLINK_SEND_TERMINAL) {
            if (correlation_released_) {
                matches = false;
                for (request_writable_wait_t::const_iterator it =
                       current->request_wait.begin ();
                     it != current->request_wait.end (); ++it) {
                    if (it->first->request_correlation_release_epoch ()
                        != it->second) {
                        matches = true;
                        break;
                    }
                }
            } else
                matches = matches && current->request_wait.empty ();
        }
        if (!matches) {
            previous = current;
            current = next;
            continue;
        }

        if (previous)
            previous->writable_wait_next = next;
        else
            state_->writable_wait_head = next;
        if (state_->writable_wait_tail == current)
            state_->writable_wait_tail = previous;
        current->writable_wait_next = NULL;
        current->writable_wait_linked = false;
        const size_t previous_count =
          state_->writable_waiting_count.fetch_sub (
            1, std::memory_order_release);
        zlink_assert (previous_count > 0);

        current->request_wait.clear ();
        current->completion.send_result = result_;
        current->completion.send_terminal_errno = terminal_errno_;
        append_ready_locked (state_, current);
        ++published;
        current = next;
    }

    if (published != 0)
        state_->changed.notify_all ();
    errno = 0;
    return published;
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
    return enqueue_ready (state_, reservation_);
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
    if (!state_->ready_head) {
        state_->ready_tail = NULL;
        state_->ready_available.store (false, std::memory_order_release);
    }
    node->ready_next = NULL;
    if (node->completion.kind == ZLINK_COMPLETION_WRITABLE) {
        const size_t previous_count = state_->ready_writable_count.fetch_sub (
          1, std::memory_order_release);
        zlink_assert (previous_count > 0);
    }
    unlink_outstanding_locked (state_, node);

    *completion_out_ = node->completion;
    node->completion.reply_parts = NULL;
    node->completion.reply_part_count = 0;
    reservation_t *node_to_delete =
      recycle_reservation_locked (state_, node);
    lock.unlock ();
    delete node_to_delete;
    errno = 0;
    return 0;
}

bool zlink::socket_completion::has_ready (queue_state_t *state_)
{
    if (!state_)
        return false;
    return state_->ready_available.load (std::memory_order_acquire);
}

bool zlink::socket_completion::has_ready_writable (queue_state_t *state_)
{
    if (!state_)
        return false;
    return state_->ready_writable_count.load (std::memory_order_acquire) != 0;
}

bool zlink::socket_completion::has_writable_wait (queue_state_t *state_)
{
    if (!state_)
        return false;
    return state_->writable_waiting_count.load (std::memory_order_acquire) != 0;
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

    reservation_t *waiter = state_->writable_wait_head;
    while (waiter) {
        reservation_t *const next = waiter->writable_wait_next;
        waiter->writable_wait_next = NULL;
        waiter->writable_wait_linked = false;
        waiter->request_wait.clear ();
        waiter->completion.send_result = ZLINK_SEND_TERMINAL;
        waiter->completion.send_terminal_errno = state_->lifecycle_errno;
        append_ready_locked (state_, waiter);
        waiter = next;
    }
    state_->writable_wait_head = NULL;
    state_->writable_wait_tail = NULL;
    state_->writable_waiting_count.store (0, std::memory_order_release);

    // A WRITABLE token can already be queued when close wins the race with
    // public dequeue. Terminalize those records too before dropping public
    // delivery so every token left on the socket reaches one terminal state.
    for (reservation_t *ready = state_->ready_head; ready;
         ready = ready->ready_next) {
        if (ready->completion.kind != ZLINK_COMPLETION_WRITABLE)
            continue;
        ready->completion.send_result = ZLINK_SEND_TERMINAL;
        ready->completion.send_terminal_errno = state_->lifecycle_errno;
    }

    // Close drops public delivery, but keeps reservation nodes alive until
    // their in-flight resolver releases them or the socket is destroyed.
    // This avoids freeing a node held by an admission attempt that lost the
    // close race after the lifecycle gate was sealed.
    state_->ready_head = NULL;
    state_->ready_tail = NULL;
    state_->ready_available.store (false, std::memory_order_release);
    state_->ready_writable_count.store (0, std::memory_order_release);
    state_->changed.notify_all ();
}
