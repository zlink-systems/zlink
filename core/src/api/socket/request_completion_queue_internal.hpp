/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_COMPLETION_QUEUE_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <mutex>
#include <thread>

namespace zlink
{
namespace request_completion
{
// One reservation is acquired before a request becomes pending and is held
// until its callback is ready to run. This bounds pending requests and queued
// terminal controls with one admission decision.
static const size_t max_pending_completions = 65536;

struct control_t
{
    control_t ();

    zlink_reply_handler_fn handler;
    void *userdata;
    int errnum;
    control_t *next;
};

struct queue_state_t
{
    queue_state_t ();
    ~queue_state_t ();

    std::mutex mutex;
    //  try_reserve() allocates one node before request admission. enqueue()
    //  moves a reserved node to this ready queue without allocating, so a
    //  terminal result cannot disappear after Core owns the request.
    control_t *reserved_head;
    control_t *ready_head;
    control_t *ready_tail;
    size_t reserved;
    std::thread::id owner_thread;
    bool owner_thread_valid;
    bool closed;
};

int enqueue (queue_state_t *state_,
             void *owner_handle_,
             zlink_reply_handler_fn handler_,
             void *userdata_,
             int errnum_,
             zlink_msg_t *parts_,
             size_t part_count_);
bool try_reserve (queue_state_t *state_);
void release_reservation (queue_state_t *state_);
void invoke_callback (void *owner_handle_,
                      zlink_reply_handler_fn handler_,
                      int errnum_,
                      zlink_msg_t *parts_,
                      size_t part_count_,
                      void *userdata_);
int drain (queue_state_t *state_, void *owner_handle_);
int drain_while_closing (queue_state_t *state_, void *owner_handle_);
void close (queue_state_t *state_);
void claim_owner_thread (queue_state_t *state_);
bool current_thread_is_owner (queue_state_t *state_);
bool has_pending (queue_state_t *state_);
bool in_request_completion_callback (void *owner_handle_);
}
}

#endif
