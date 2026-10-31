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
    // Close now owns the socket. Join a running inproc reconnect callback and
    // reject later ones before monitor cleanup, async quiescence or reap. The
    // caller holds no socket lock here, so the join cannot wait on itself.
    stop_inproc_reconnect_scheduler ();
    {
        // Internal command work uses the socket turn without a public
        // admission. Join it after stopping reconnect callbacks and before
        // teardown; later lease acquisitions observe closing in this turn.
        socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
        socket_public_api_lock_scope_t command_owner (
          lifecycle, !lifecycle.public_api_sync_owned_by_current_thread ());
    }
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
