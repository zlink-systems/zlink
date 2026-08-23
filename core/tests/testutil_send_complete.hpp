/* SPDX-License-Identifier: MPL-2.0 */

//  Shared helpers for the send-completion contract.
//
//  These replace the readiness-hint scaffolding the tests used before: where a
//  test previously installed a readiness handler and waited for an edge,
//  it now installs a completion handler and waits for the operation's single
//  completion. The observable being asserted is the same - "the socket's async
//  callback consumer ran on a Core-owned thread" - but it is now tied to a
//  concrete operation instead of a hint.

#ifndef ZLINK_TESTUTIL_SEND_COMPLETE_HPP_INCLUDED
#define ZLINK_TESTUTIL_SEND_COMPLETE_HPP_INCLUDED

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "zlink.h"

namespace zlink_test
{
struct send_complete_event_copy_t
{
    send_complete_event_copy_t () :
        op_id (0),
        userdata (NULL),
        transport_pair_id (0),
        transport_pair_generation (0),
        result (ZLINK_SEND_ADMITTED),
        terminal_errno (0)
    {
    }

    zlink_send_op_id_t op_id;
    void *userdata;
    std::string peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    zlink_send_complete_result_t result;
    int terminal_errno;
};

//  Records every completion in arrival order. Arrival order is part of the
//  contract for one target, so the vector is the assertion surface.
struct send_complete_probe_t
{
    send_complete_probe_t () : count (0), admitted (0), timed_out (0), terminal (0) {}

    std::atomic<int> count;
    std::atomic<int> admitted;
    std::atomic<int> timed_out;
    std::atomic<int> terminal;
    zlink::mutex_t sync;
    std::vector<send_complete_event_copy_t> events;
};

inline void record_send_complete (void *,
                                  const zlink_send_complete_event_t *event_,
                                  void *userdata_)
{
    send_complete_probe_t *probe =
      static_cast<send_complete_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    send_complete_event_copy_t copy;
    copy.op_id = event_->op_id;
    copy.userdata = event_->userdata;
    copy.peer_rid.assign (
      reinterpret_cast<const char *> (event_->peer_rid.data),
      event_->peer_rid.size);
    copy.transport_pair_id = event_->transport_pair_id;
    copy.transport_pair_generation = event_->transport_pair_generation;
    copy.result = event_->result;
    copy.terminal_errno = event_->terminal_errno;
    {
        zlink::scoped_lock_t lock (probe->sync);
        probe->events.push_back (copy);
    }
    if (event_->result == ZLINK_SEND_ADMITTED)
        probe->admitted.fetch_add (1, std::memory_order_acq_rel);
    else if (event_->result == ZLINK_SEND_TIMED_OUT)
        probe->timed_out.fetch_add (1, std::memory_order_acq_rel);
    else
        probe->terminal.fetch_add (1, std::memory_order_acq_rel);
    probe->count.fetch_add (1, std::memory_order_acq_rel);
}

//  Discards the event. Used where the test only needs the socket to own an
//  async completion consumer.
inline void ignore_send_complete (void *,
                                  const zlink_send_complete_event_t *,
                                  void *)
{
}

inline zlink_send_async_options_t make_send_async_options (
  const zlink_routed_submit_target_t *target_ = NULL,
  uint32_t timeout_ms_ = 0,
  void *userdata_ = NULL)
{
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.timeout_ms = timeout_ms_;
    options.userdata = userdata_;
    options.target = target_;
    return options;
}

//  Submits one single-part record built from `data_`.
inline zlink_submit_result_t send_async_bytes (
  void *socket_, const void *data_, size_t size_,
  const zlink_send_async_options_t *options_, zlink_send_op_id_t *op_id_out_)
{
    zlink_msg_t part;
    if (zlink_msg_init_size (&part, size_) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    if (size_ > 0)
        memcpy (zlink_msg_data (&part), data_, size_);
    const zlink_submit_result_t rc =
      zlink_send_async (socket_, &part, 1, options_, op_id_out_);
    if (rc != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return rc;
}
}

#endif
