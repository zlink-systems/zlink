/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace zlink
{
namespace socket_completion
{
// A reservation is counted from successful FINAL acceptance until the public
// completion record is dequeued.  Terminal publication is allocation-free.
static const size_t max_outstanding_completions = 65536;
// Keep a bounded set of dequeued/cancelled reservation nodes. Request
// completions are normally consumed and replaced continuously, so recycling
// the nodes removes allocator traffic without retaining memory proportional
// to the configured outstanding limit.
static const size_t inline_cached_reservations = 64;
static const size_t max_cached_reservations = max_outstanding_completions;

struct reservation_t
{
    reservation_t ();

    zlink_completion_t completion;
    reservation_t *ready_next;
    reservation_t *all_previous;
    reservation_t *all_next;
    bool ready;
    bool heap_owned;
};

struct queue_state_t
{
    queue_state_t ();
    ~queue_state_t ();

    std::mutex mutex;
    std::condition_variable changed;
    reservation_t *ready_head;
    reservation_t *ready_tail;
    reservation_t *all_head;
    reservation_t *cached_head;
    reservation_t inline_cache[inline_cached_reservations];
    size_t outstanding;
    size_t cached;
    uint64_t next_id;
    int lifecycle_errno;
};

int reserve (queue_state_t *state_,
             zlink_completion_kind_t kind_,
             void *user_context_,
             const zlink_routing_id_t *peer_rid_,
             reservation_t **reservation_out_,
             zlink_completion_id_t *completion_id_out_);
void release (queue_state_t *state_, reservation_t *reservation_);
int publish_send (queue_state_t *state_,
                  reservation_t *reservation_,
                  zlink_send_complete_result_t result_,
                  int terminal_errno_);
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
size_t outstanding (queue_state_t *state_);
void close (queue_state_t *state_, int lifecycle_errno_);
}
}

#endif
