/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_REPLY_RUNTIME_CORE_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_REPLY_RUNTIME_CORE_HPP_INCLUDED__

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/request_timeout_scheduler_internal.hpp"

#include <errno.h>
#include <memory>
#include <stdint.h>
#include <unordered_set>

namespace zlink
{
namespace request_reply_runtime
{
struct sequence_state_t
{
    sequence_state_t () :
        default_timeout_ms (zlink::request_reply::default_timeout_ms), next_request_seq (1)
    {
    }

    uint32_t default_timeout_ms;
    uint64_t next_request_seq;
    std::unordered_set<uint64_t> pending_sequences;
};

template <typename PendingSet>
inline uint64_t allocate_request_sequence (uint64_t *next_request_seq_,
                                           const PendingSet &pending_sequences_)
{
    if (!next_request_seq_) {
        errno = EFAULT;
        return 0;
    }

    const uint64_t start = *next_request_seq_ == 0 ? 1 : *next_request_seq_;
    uint64_t candidate = start;

    do {
        if (candidate == 0)
            candidate = 1;

        if (pending_sequences_.count (candidate) == 0) {
            uint64_t next = candidate + 1;
            if (next == 0)
                next = 1;
            *next_request_seq_ = next;
            return candidate;
        }

        ++candidate;
        if (candidate == 0)
            candidate = 1;
    } while (candidate != start);

    errno = EBUSY;
    return 0;
}

template <typename SequenceState> inline uint64_t allocate_request_sequence (SequenceState *state_)
{
    if (!state_) {
        errno = EFAULT;
        return 0;
    }

    return allocate_request_sequence (&state_->next_request_seq, state_->pending_sequences);
}

template <typename CallbackContext> inline void destroy_timeout_callback_ctx (void *userdata_)
{
    delete static_cast<CallbackContext *> (userdata_);
}

template <typename CallbackContext, typename InitFn>
inline int schedule_timeout_task (uint32_t timeout_ms_,
                                  void (*callback_) (void *),
                                  InitFn init_context_,
                                  std::shared_ptr<zlink::request_timeout::task_t> *task_out_)
{
    if (!callback_ || !task_out_) {
        errno = EFAULT;
        return -1;
    }

    std::unique_ptr<CallbackContext> timeout_ctx (new (std::nothrow) CallbackContext ());
    if (!timeout_ctx) {
        errno = ENOMEM;
        return -1;
    }

    init_context_ (*timeout_ctx);
    *task_out_ = zlink::request_timeout::schedule (timeout_ms_, callback_, timeout_ctx.release (),
                                                   &destroy_timeout_callback_ctx<CallbackContext>);
    if (!*task_out_) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}
}
}

#endif
