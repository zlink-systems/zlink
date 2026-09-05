/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zlink
{
class pipe_t;
namespace socket_completion
{
// Refused candidates and their pipe-owned reservation-return edge. No payload.
typedef std::vector<std::pair<std::shared_ptr<pipe_t>, uint64_t> >
  request_writable_wait_t;
// A reservation is counted from REQUEST FINAL acceptance or WRITABLE wait-token
// issuance until the public completion record is dequeued. Terminal publication
// is allocation-free.
static const size_t max_outstanding_completions = 65536;
// Keep dequeued/cancelled reservation nodes for reuse. The cache grows only
// to observed concurrent demand instead of eagerly allocating the outstanding
// limit, while steady request/reply traffic avoids allocator churn.
static const size_t inline_cached_reservations = 64;
static const size_t max_cached_reservations = max_outstanding_completions;

struct reservation_t
{
    reservation_t ();

    zlink_completion_t completion;
    reservation_t *ready_next;
    reservation_t *writable_wait_next;
    reservation_t *outstanding_previous;
    reservation_t *outstanding_next;
    bool ready;
    bool writable_wait_linked;
    bool heap_owned;
    request_writable_wait_t request_wait;
};

struct queue_state_t
{
    queue_state_t ();
    ~queue_state_t ();

    std::mutex mutex;
    std::condition_variable changed;
    reservation_t *ready_head;
    reservation_t *ready_tail;
    reservation_t *writable_wait_head;
    reservation_t *writable_wait_tail;
    reservation_t *outstanding_head;
    reservation_t *cached_head;
    reservation_t inline_cache[inline_cached_reservations];
    size_t outstanding;
    size_t cached;
    uint64_t next_id;
    int lifecycle_errno;
    std::atomic<bool> ready_available;
    std::atomic<size_t> ready_writable_count;
    std::atomic<size_t> writable_waiting_count;
};

void release_payload (zlink_completion_t *completion_);

int reserve (queue_state_t *state_,
             zlink_completion_kind_t kind_,
             void *user_context_,
             const zlink_routing_id_t *peer_rid_,
             reservation_t **reservation_out_,
             zlink_completion_id_t *completion_id_out_);
int reserve_writable_wait (queue_state_t *state_,
                           void *user_context_,
                           const zlink_routing_id_t *peer_rid_,
                           reservation_t **reservation_out_,
                           zlink_completion_id_t *completion_id_out_,
                           request_writable_wait_t *request_wait_ = NULL);
void release (queue_state_t *state_, reservation_t *reservation_);
// Physical credit and terminal publication match the target (NULL means the
// size-zero PAIR/DEALER group). Correlation publication instead checks each
// refused pipe's return edge, independently of physical readiness and routes.
int publish_writable_waiters (queue_state_t *state_,
                              const zlink_routing_id_t *target_rid_or_null_,
                              zlink_send_complete_result_t result_,
                              int terminal_errno_,
                              bool correlation_released_ = false);
int publish_request (queue_state_t *state_,
                     reservation_t *reservation_,
                     zlink_request_result_t result_,
                     zlink_msg_t *parts_,
                     size_t part_count_);
int recv (queue_state_t *state_,
          zlink_completion_t *completion_out_,
          zlink_recv_flags_t flags_,
          int timeout_ms_);
bool has_ready (queue_state_t *state_);
bool has_ready_writable (queue_state_t *state_);
bool has_writable_wait (queue_state_t *state_);
size_t outstanding (queue_state_t *state_);
void close (queue_state_t *state_, int lifecycle_errno_);
}
}

#endif
