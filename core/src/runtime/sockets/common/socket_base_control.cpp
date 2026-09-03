/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"

uint32_t zlink::socket_base_t::local_peer_weight () const
{
    return _local_peer_weight.load (std::memory_order_relaxed);
}

int zlink::socket_base_t::close ()
{
    return close (-1);
}

int zlink::socket_base_t::close (int handoff_timeout_ms_)
{
    const int admission = begin_close_handoff ();
    if (admission < 0)
        return -1;
    if (admission > 0)
        return 0;

    finish_close_handoff (handoff_timeout_ms_);
    return 0;
}

int zlink::socket_base_t::begin_close_handoff ()
{
    if (!lifecycle_coordinator ().begin_close_or_fail_busy ())
        return -1;
    // A blocking complete-record submit deliberately drops its public
    // admission while parked so close remains admissible. Publish the close
    // edge to both progress-owner forms: the async owner sleeps on the
    // progress CV, while a zero-I/O-thread caller owns the mailbox directly.
    notify_submit_progress ();
    // A zero-I/O-thread waiter can publish itself after the progress epoch
    // advances but before it starts the mailbox wait. Signal unconditionally
    // on this one-shot cold edge so that handoff cannot be lost in that gap.
    static_cast<mailbox_t *> (_mailbox)->signal ();
    return 0;
}

void zlink::socket_base_t::complete_close_handoff ()
{
    finish_close_handoff ();
}
