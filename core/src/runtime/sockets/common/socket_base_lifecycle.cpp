/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <boost/asio.hpp>

#include "core/io_thread.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_public_handle.hpp"

namespace
{
zlink::socket_base_t *&async_mailbox_dispatch_socket_tls ()
{
    static thread_local zlink::socket_base_t *socket = NULL;
    return socket;
}

bool is_pair_pipe_lifetime_command (const zlink::command_t &cmd_)
{
    return cmd_.type == zlink::command_t::bind
           || cmd_.type == zlink::command_t::pipe_term_ack;
}

class command_drain_active_guard_t
{
  public:
    explicit command_drain_active_guard_t (std::atomic<bool> &active_) :
        _active (active_), _published (false)
    {
    }

    ~command_drain_active_guard_t ()
    {
        release ();
    }

    void publish ()
    {
        if (_published)
            return;
        _active.store (true, std::memory_order_release);
        _published = true;
    }

    void release ()
    {
        if (!_published)
            return;
        _active.store (false, std::memory_order_release);
        _published = false;
    }

  private:
    std::atomic<bool> &_active;
    bool _published;
};

#ifdef ZLINK_BUILD_TESTS
std::atomic<zlink::async_owner_transition_test_hook_fn>
  async_owner_transition_test_hook (NULL);
std::atomic<void *> async_owner_transition_test_hook_userdata (NULL);

void invoke_async_owner_transition_test_hook (
  zlink::async_owner_transition_test_point_t point_)
{
    const zlink::async_owner_transition_test_hook_fn hook =
      async_owner_transition_test_hook.load (std::memory_order_acquire);
    if (hook)
        hook (point_, async_owner_transition_test_hook_userdata.load (
                        std::memory_order_acquire));
}
#endif
}

#ifdef ZLINK_BUILD_TESTS
void zlink::test_set_async_owner_transition_hook (
  async_owner_transition_test_hook_fn hook_, void *userdata_)
{
    if (!hook_) {
        async_owner_transition_test_hook.store (NULL,
                                                std::memory_order_release);
        async_owner_transition_test_hook_userdata.store (
          NULL, std::memory_order_release);
        return;
    }
    async_owner_transition_test_hook_userdata.store (
      userdata_, std::memory_order_release);
    async_owner_transition_test_hook.store (hook_, std::memory_order_release);
}
#endif

void zlink::socket_base_t::reaper_mailbox_handler (void *arg_)
{
    socket_base_t *self = static_cast<socket_base_t *> (arg_);
    self->in_event ();
    self->dec_mailbox_ref ();
}

void zlink::socket_base_t::reaper_mailbox_pre_post (void *arg_)
{
    socket_base_t *self = static_cast<socket_base_t *> (arg_);
    self->inc_mailbox_ref ();
}

void zlink::socket_base_t::async_mailbox_handler (void *arg_)
{
    socket_base_t *self = static_cast<socket_base_t *> (arg_);
    socket_base_t *previous = async_mailbox_dispatch_socket_tls ();
    async_mailbox_dispatch_socket_tls () = self;
    self->process_async_mailbox ();
    async_mailbox_dispatch_socket_tls () = previous;
    self->dec_mailbox_ref ();
}

void zlink::socket_base_t::async_mailbox_pre_post (void *arg_)
{
    socket_base_t *self = static_cast<socket_base_t *> (arg_);
    self->inc_mailbox_ref ();
}

zlink::socket_base_t *zlink::socket_base_t::current_async_mailbox_dispatch_socket ()
{
    return async_mailbox_dispatch_socket_tls ();
}

void zlink::socket_base_t::start_reaping (poller_t *poller_)
{
    //  The mailbox must have exactly one executor owner. The reaper is the
    //  ownership boundary that waits until the old executor has detached.
    if (lifecycle_coordinator ().is_async_quiesce_pending ())
        wait_async_quiesced (-1);

    //  Plug the socket to the reaper thread.
    lifecycle_coordinator ().set_reaper_poller (poller_);

    mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
    mailbox->set_io_context (&lifecycle_coordinator ().reaper_poller ()->get_io_context (),
                             &socket_base_t::reaper_mailbox_handler, this,
                             &socket_base_t::reaper_mailbox_pre_post);
    mailbox->schedule_if_needed ();

    //  Initialise the termination and check whether it can be deallocated
    //  immediately.
    terminate ();
    check_destroy ();
}

int zlink::socket_base_t::process_commands (
  int timeout_, bool throttle_, bool force_if_command_pending_)
{
    receive_runtime_t &receive = receive_runtime ();
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    mailbox_t *const mailbox = static_cast<mailbox_t *> (_mailbox);
    const bool async_executor = current_async_mailbox_dispatch_socket () == this;
    const uint64_t wait_deadline =
      timeout_ > 0 ? _clock.now_ms () + static_cast<uint64_t> (timeout_) : 0;
    bool pair_pipe_lifetime_sync_required = false;
    bool woke_since_last_claim = false;
    command_drain_active_guard_t command_drain_active (
      receive.command_drain_active);

    while (true) {
        // Once the mailbox handoff begins, the async executor is the sole
        // command progress owner. A public path must not wait for that owner
        // while it can re-enter this socket during command dispatch.
        if (async_mailbox_owns_commands () && !async_executor) {
            if (_ctx_terminated) {
                errno = ETERM;
                return -1;
            }
            return 0;
        }

        bool processed_command = false;
        bool retry_pair_pipe_lifetime_with_sync = false;
        bool submit_progress_tracked = false;
        uint64_t submit_progress_epoch_before = 0;
        {
            // Socket commands can mutate non-PAIR distribution state used by
            // concurrent sends. The exact calling thread, rather than the
            // process-wide held bit, decides whether this scope must acquire
            // public API synchronization. PAIR activation commands remain
            // lock-free; a front bind/termination transition is probed under
            // command ownership and retried in API -> command-owner order.
            const bool pair_sync_for_this_claim =
              pair_pipe_lifetime_sync_required;
            pair_pipe_lifetime_sync_required = false;
            // Once start_reaping() installs the reaper poller, public close
            // cleanup and async quiescence are complete and no send can resume.
            // A parked marker may still leave a stale raw bit, so the reaper
            // must not wait on it. Before that ownership boundary, including
            // async close handoff, command mutation still fences close-time
            // multipart rollback with the ordinary lifecycle sync.
            const bool reaper_owns_closed_socket =
              lifecycle.public_close_requested ()
              && lifecycle.reaper_poller () != NULL;
            const bool command_path_needs_public_api_sync =
              !reaper_owns_closed_socket
              && ((options.type != ZLINK_CORE_SOCKET_PAIR)
                  || pair_sync_for_this_claim);
            const bool acquire_public_api_sync =
              command_path_needs_public_api_sync
              && !lifecycle.public_api_sync_owned_by_current_thread ();
            socket_public_api_lock_scope_t api_owner (
              lifecycle, acquire_public_api_sync);
            scoped_lock_t command_owner (receive.command_owner_sync);

            // Close the handoff race between the lock-free owner check above
            // and acquiring the command scope. The pending bit remains set
            // until the async owner has been installed.
            if (async_mailbox_owns_commands () && !async_executor) {
                if (_ctx_terminated) {
                    errno = ETERM;
                    return -1;
                }
                return 0;
            }

            bool took_command_pending_hint = false;
            if (!pair_sync_for_this_claim && timeout_ == 0 && throttle_
                && force_if_command_pending_) {
                // Direct request-reply commit points need a command drain only
                // when a sender woke an inactive mailbox.
                if (!mailbox->has_command_pending_hint ())
                    return 0;
                // Publish the claim before clearing its pending hint. A
                // concurrent direct-submit progress point therefore observes
                // either the queued hint or this in-flight drain until every
                // command-side state mutation below has completed.
                command_drain_active.publish ();
                took_command_pending_hint =
                  mailbox->take_command_pending_hint ();
                if (!took_command_pending_hint)
                    return 0;
            }
            if (!pair_sync_for_this_claim && timeout_ == 0 && throttle_) {
                const uint64_t tsc = zlink::clock_t::rdtsc ();
                const bool should_skip =
                  tsc
                  && command_runtime ().should_skip_throttled_command_poll (
                    tsc);
                if (should_skip && !took_command_pending_hint)
                    return 0;
            }

            // Clear a consumed wake hint before draining. A concurrent enqueue
            // either joins this drain or publishes a hint for the next commit.
            command_drain_active.publish ();
            if (!took_command_pending_hint)
                (void) mailbox->take_command_pending_hint ();

            // Direct transition callbacks below may already publish the exact
            // route/credit edge. Snapshot only while a reply is waiting so the
            // end-of-batch catch-all can avoid broadcasting that edge twice.
            socket_submit_progress_runtime_t &submit_progress =
              submit_progress_runtime ();
            if (submit_progress.waiters.load (std::memory_order_acquire) != 0) {
                scoped_lock_t progress_lock (submit_progress.sync);
                if (submit_progress.waiters.load (
                      std::memory_order_relaxed)
                    != 0) {
                    submit_progress_tracked = true;
                    submit_progress_epoch_before =
                      submit_progress.epoch.load (std::memory_order_acquire);
                }
            }

            command_t cmd;
            const bool probe_pair_lifetime_command =
              options.type == ZLINK_CORE_SOCKET_PAIR
              && !reaper_owns_closed_socket
              && !lifecycle.public_api_sync_owned_by_current_thread ();
            const auto recv_next_command = [&] (command_t *cmd_out_) {
                if (probe_pair_lifetime_command) {
                    const mailbox_t::command_probe_result_t probe =
                      mailbox->probe_command (&is_pair_pipe_lifetime_command);
                    if (probe == mailbox_t::command_probe_empty) {
                        errno = EAGAIN;
                        return -1;
                    }
                    if (probe == mailbox_t::command_probe_match) {
                        retry_pair_pipe_lifetime_with_sync = true;
                        errno = EAGAIN;
                        return -1;
                    }
                }
                return mailbox->recv (cmd_out_, 0);
            };

#ifdef ZLINK_BUILD_TESTS
            if (async_executor)
                receive.async_mailbox_drains.fetch_add (
                  1, std::memory_order_relaxed);
            else
                receive.public_mailbox_drains.fetch_add (
                  1, std::memory_order_relaxed);
#endif

            int rc = recv_next_command (&cmd);
            if (rc != 0 && errno == EINTR)
                return -1;

            while (rc == 0 || errno == EINTR) {
                if (rc == 0) {
#ifdef ZLINK_BUILD_TESTS
                    receive_runtime_t::command_sync_probe_hook_fn sync_probe =
                      receive.command_sync_probe_hook.load (
                        std::memory_order_acquire);
                    if (sync_probe) {
                        const bool acquired = receive.sync.try_lock ();
                        if (acquired)
                            receive.sync.unlock ();
                        sync_probe (
                          receive.command_sync_probe_userdata.load (
                            std::memory_order_acquire),
                          static_cast<int> (cmd.type), !acquired,
                          lifecycle.public_api_sync_owned_by_current_thread ());
                    }
#endif
                    {
                        scoped_lock_t receive_owner (receive.sync);
                        cmd.destination->process_command (cmd);
                    }
                    // Pipe termination notifications defer routing
                    // cleanup until the command's receive scope is gone.
                    process_deferred_socket_msg_pipe_terminations ();
                    processed_command = true;
                }
                rc = recv_next_command (&cmd);
            }

            // Publish readiness only after the command owner applied every
            // state transition in this batch.
            if (processed_command) {
                mailbox->signal_pollers ();
                flush_deferred_peer_controls ();
                if (submit_progress_tracked) {
                    scoped_lock_t progress_lock (submit_progress.sync);
                    if (submit_progress.epoch.load (std::memory_order_acquire)
                        == submit_progress_epoch_before) {
                        submit_progress.epoch.fetch_add (
                          1, std::memory_order_release);
                        submit_progress.cv.broadcast ();
                    }
                } else
                    notify_submit_progress ();
            }

            zlink_assert (errno == EAGAIN);
            if (_ctx_terminated) {
                errno = ETERM;
                return -1;
            }
            if (!retry_pair_pipe_lifetime_with_sync
                && (processed_command || woke_since_last_claim
                    || timeout_ == 0))
                return 0;
            if (!retry_pair_pipe_lifetime_with_sync)
                command_drain_active.release ();
        }

        if (retry_pair_pipe_lifetime_with_sync) {
            pair_pipe_lifetime_sync_required = true;
            continue;
        }

        // Blocking recv must not retain public API synchronization while it
        // sleeps. A notification is only a hint; the next loop claims the
        // command under API -> command-owner -> receive lock order.
        int wait_timeout = timeout_;
        if (timeout_ > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= wait_deadline)
                return 0;
            wait_timeout = static_cast<int> (wait_deadline - now);
        }
        if (mailbox->wait_for_command_signal (wait_timeout) != 0) {
            if (errno == EINTR)
                return -1;
            zlink_assert (errno == EAGAIN);
            return 0;
        }
        // signal() is also a commandless progress edge (for example a newly
        // writable attach). Preserve recv(timeout)'s legacy behavior: one
        // empty claim after this wake returns to the caller so it can retry
        // its socket operation instead of sleeping again indefinitely.
        woke_since_last_claim = true;
    }
}

int zlink::socket_base_t::process_submit_commands ()
{
    //  This is an explicit commit point before direct transport access. Bypass
    //  a throttled skip only when send() published an actual command batch;
    //  the common no-command reply path remains syscall-free.
    mailbox_t *const mailbox = static_cast<mailbox_t *> (_mailbox);
    if (!mailbox->has_command_pending_hint ()
        && !receive_runtime ().command_drain_active.load (
          std::memory_order_acquire)) {
        if (_ctx_terminated.load (std::memory_order_acquire)) {
            errno = ETERM;
            return -1;
        }
        return 0;
    }
    const int rc = process_commands (0, true, true);
    if (rc == 0 && _ctx_terminated.load (std::memory_order_acquire)) {
        errno = ETERM;
        return -1;
    }
    return rc;
}

uint64_t zlink::socket_base_t::observe_submit_progress () const
{
    return _runtime.submit_progress_runtime.epoch.load (
      std::memory_order_acquire);
}

int zlink::socket_base_t::wait_submit_progress (
  uint64_t observed_epoch_, int timeout_ms_,
  bool *owner_progress_held_)
{
    if (!owner_progress_held_) {
        errno = EFAULT;
        return -1;
    }

    socket_submit_progress_runtime_t &progress =
      submit_progress_runtime ();
    {
        scoped_lock_t lock (progress.sync);
        progress.waiters.fetch_add (1, std::memory_order_seq_cst);
    }

    int rc = 0;
    bool synchronous_progress = false;
    if (!*owner_progress_held_) {
        // Once a public reply actually blocks, command progress is part of the
        // socket's steady-state transport work. Keep its executor for the
        // socket lifetime instead of detaching and restarting it at every HWM
        // recovery edge.
        retain_async_command_processing ();
        rc = acquire_transport_pair_owner_progress ();
        if (rc == 0)
            *owner_progress_held_ = true;
        else if (errno == EAGAIN) {
            // A zero-I/O-thread context is valid for inproc. With no async
            // executor available, this public caller becomes the temporary
            // command owner while its send scope is unlocked. This preserves
            // finite/infinite SNDTIMEO semantics without polling slices.
            rc = process_commands (timeout_ms_, false);
            synchronous_progress = true;
        }
    }

    if (rc == 0 && !synchronous_progress) {
        const uint64_t deadline =
          timeout_ms_ > 0
            ? _clock.now_ms () + static_cast<uint64_t> (timeout_ms_)
            : 0;
        progress.sync.lock ();
        while (progress.epoch.load (std::memory_order_seq_cst)
               == observed_epoch_) {
            int wait_timeout = timeout_ms_;
            if (timeout_ms_ > 0) {
                const uint64_t now = _clock.now_ms ();
                if (now >= deadline) {
                    errno = EAGAIN;
                    rc = -1;
                    break;
                }
                wait_timeout = static_cast<int> (deadline - now);
            }
            rc = progress.cv.wait (&progress.sync, wait_timeout);
            if (rc != 0)
                break;
        }
        progress.sync.unlock ();
    }

    progress.waiters.fetch_sub (1, std::memory_order_seq_cst);
    return rc;
}

int zlink::socket_base_t::wait_submit_progress (
  socket_public_send_scope_t &send_scope_, uint64_t observed_epoch_,
  int timeout_ms_, bool *owner_progress_held_)
{
    send_scope_.release_sync_for_retry ();
    const int rc = wait_submit_progress (
      observed_epoch_, timeout_ms_, owner_progress_held_);
    const int wait_errno = errno;
    send_scope_.reacquire_sync_after_retry ();
    errno = wait_errno;
    return rc;
}

void zlink::socket_base_t::notify_submit_progress ()
{
    socket_submit_progress_runtime_t &progress =
      submit_progress_runtime ();
    // Publish every transition, including the interval before a failed
    // admission has installed its waiter. The submitter snapshots this epoch
    // before trying the pipe, so an activation racing the failure cannot be
    // mistaken for the state it should wait on.
    progress.epoch.fetch_add (1, std::memory_order_seq_cst);
    const uint32_t waiters =
      progress.waiters.load (std::memory_order_seq_cst);
    if (waiters == 0)
        return;

    scoped_lock_t lock (progress.sync);
    progress.cv.broadcast ();
}

#ifdef ZLINK_BUILD_TESTS
void zlink::socket_base_t::test_receive_owner_snapshot (
  uint64_t *progress_epoch_out_, uint64_t *public_mailbox_drains_out_,
  uint64_t *async_mailbox_drains_out_)
{
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t receive_lock (receive.sync);
    if (progress_epoch_out_)
        *progress_epoch_out_ = receive.progress_epoch;
    if (public_mailbox_drains_out_)
        *public_mailbox_drains_out_ =
          receive.public_mailbox_drains.load (std::memory_order_acquire);
    if (async_mailbox_drains_out_)
        *async_mailbox_drains_out_ =
          receive.async_mailbox_drains.load (std::memory_order_acquire);
}

void zlink::socket_base_t::test_set_receive_wait_hook (
  receive_runtime_t::wait_hook_fn hook_, void *userdata_)
{
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t receive_lock (receive.sync);
    receive.wait_hook = hook_;
    receive.wait_hook_userdata = userdata_;
}

void zlink::socket_base_t::test_set_receive_record_hooks (
  receive_runtime_t::record_hook_fn acquired_hook_,
  receive_runtime_t::record_hook_fn contention_hook_, void *userdata_)
{
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t receive_lock (receive.sync);
    receive.record_hook_userdata.store (userdata_, std::memory_order_release);
    receive.record_acquired_hook.store (acquired_hook_,
                                        std::memory_order_release);
    receive.record_contention_hook.store (contention_hook_,
                                          std::memory_order_release);
}

void zlink::socket_base_t::test_set_receive_command_sync_probe_hook (
  receive_runtime_t::command_sync_probe_hook_fn hook_, void *userdata_)
{
    receive_runtime_t &receive = receive_runtime ();
    if (!hook_)
        receive.command_sync_probe_hook.store (NULL,
                                               std::memory_order_release);
    receive.command_sync_probe_userdata.store (userdata_,
                                               std::memory_order_release);
    if (hook_)
        receive.command_sync_probe_hook.store (hook_,
                                               std::memory_order_release);
}
#endif

int zlink::socket_base_t::start_async_mailbox_processing (io_thread_t *io_thread_)
{
    receive_runtime_t &receive = receive_runtime ();
    mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
    // Block new lock-free receive attempts before installing the async owner.
    // A receive that already passed its second check must complete before the
    // executor can enter command delivery under receive.sync.
    receive.require_receive_sync_for_async_owner ();
    receive.async_command_handoff_pending.store (true, std::memory_order_release);
    // Wake the former public owner before waiting for its command scope.  The
    // pending bit prevents a second public waiter from taking ownership while
    // this handoff is in flight.
    mailbox->signal ();

    int rc = 0;
    {
        scoped_lock_t command_owner (receive.command_owner_sync);
        rc = lifecycle_coordinator ().start_async_mailbox_processing (
          mailbox, io_thread_, &socket_base_t::async_mailbox_handler, this,
          &socket_base_t::async_mailbox_pre_post);
        receive.async_command_handoff_pending.store (false, std::memory_order_release);
    }
    if (rc == 0) {
        // Make the ownership transition visible to receivers that are already
        // waiting on the separate receive-progress channel.
        notify_receive_progress ();
    } else {
        receive.release_receive_sync_from_async_owner ();
        // A public waiter may have observed the pending handoff and switched
        // to the progress channel. Return it to the primary mailbox owner.
        notify_receive_progress ();
        mailbox->signal ();
    }
    return rc;
}

void zlink::socket_base_t::stop_async_mailbox_processing ()
{
    receive_runtime_t &receive = receive_runtime ();
    {
        // External completion-owner handoff is the one stop path that does
        // not already own the progress gate. Serialize its lifecycle change
        // with every temporary-owner start so an old handler cannot detach a
        // newly installed mailbox context. The idle-stop path calls the
        // coordinator directly because it already holds this gate.
        scoped_lock_t progress_lock (
          _transport_pair_owner_progress_sync);
        scoped_lock_t command_owner (receive.command_owner_sync);
        if (_completion_poller_refs.load (std::memory_order_acquire) == 0)
            invalidate_completion_processing_owner ();
        lifecycle_coordinator ().stop_async_mailbox_processing (
          static_cast<mailbox_t *> (_mailbox));
    }
    notify_receive_progress ();
    static_cast<mailbox_t *> (_mailbox)->signal ();
}

void zlink::socket_base_t::wait_async_quiesced (int timeout_ms_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    // async_processing_done describes the currently installed executor and is
    // reset when a waiter starts its replacement. The stop handoff itself is
    // complete once async_quiesce_pending clears, so wait on that stable
    // predicate; otherwise a late-waking waiter can sleep for its full timeout
    // behind the newly started executor.
    if (!lifecycle.is_async_quiesce_pending ())
        return;

    scoped_lock_t lock (lifecycle.async_done_mu);
    const int wait_timeout_ms =
      timeout_ms_ < 0 ? -1 : timeout_ms_ > 0 ? timeout_ms_ : 2000;
    while (lifecycle.is_async_quiesce_pending ()) {
        const int rc = lifecycle.async_done_cv.wait (
          &lifecycle.async_done_mu, wait_timeout_ms);
        if (rc != 0)
            break;
    }
}

void zlink::socket_base_t::retain_async_command_processing ()
{
    scoped_lock_t progress_lock (_transport_pair_owner_progress_sync);
    _async_command_processing_retained.store (true,
                                               std::memory_order_release);
    _async_command_processing_stop_requested = false;
}

int zlink::socket_base_t::ensure_async_command_processing (bool retain_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    for (;;) {
        bool wait_for_quiescence = false;
        bool started_here = false;
        {
            socket_public_api_scope_t admission (lifecycle);
            if (!admission.acquired ())
                return -1;

            socket_public_api_lock_scope_t guard (lifecycle);
            scoped_lock_t progress_lock (
              _transport_pair_owner_progress_sync);
            if (retain_) {
                _async_command_processing_retained.store (
                  true, std::memory_order_release);
                _async_command_processing_stop_requested = false;
            }
            if (lifecycle.is_async_quiesce_pending ()) {
                wait_for_quiescence = true;
            } else if (lifecycle.is_async_mailbox_active ()) {
                return 0;
            } else {
                io_thread_t *io_thread = choose_io_thread (options.affinity);
                if (!io_thread) {
                    errno = EAGAIN;
                    return -1;
                }
                if (start_async_mailbox_processing (io_thread) != 0)
                    return -1;
                started_here = true;
            }
        }

        if (wait_for_quiescence) {
            wait_async_quiesced (10000);
            if (lifecycle.is_async_quiesce_pending ()) {
                errno = EBUSY;
                return -1;
            }
            continue;
        }
        if (started_here)
            lifecycle.wait_async_started (1000);
        return 0;
    }
}

int zlink::socket_base_t::acquire_monitor_async_command_processing ()
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();

    // A temporary owner can be in the short stop->mailbox-detach interval.
    // Do not install a second executor on the same mailbox; wait without the
    // public API lock so the retiring handler can finish its command scope.
    for (;;) {
        bool wait_for_quiescence = false;
        bool started_here = false;
        {
            socket_public_api_scope_t admission (lifecycle);
            if (!admission.acquired ())
                return -1;

            socket_public_api_lock_scope_t guard (lifecycle);
#ifdef ZLINK_BUILD_TESTS
            invoke_async_owner_transition_test_hook (
              async_owner_test_monitor_acquire_before_gate);
#endif
            scoped_lock_t progress_lock (
              _transport_pair_owner_progress_sync);
            if (lifecycle.is_async_quiesce_pending ()) {
                wait_for_quiescence = true;
            } else {
                // The flag is a lease, not a record of which consumer happened
                // to start the executor. Publishing it under the owner gate
                // cancels an idle-stop request before that request can detach.
                monitor_runtime ().owns_async_command_processing.store (
                  true, std::memory_order_release);
                _async_command_processing_stop_requested = false;
                if (lifecycle.is_async_mailbox_active ())
                    return 0;

                io_thread_t *io_thread = choose_io_thread (options.affinity);
                if (!io_thread) {
                    monitor_runtime ().owns_async_command_processing.store (
                      false, std::memory_order_release);
                    errno = EAGAIN;
                    return -1;
                }
                if (start_async_mailbox_processing (io_thread) != 0) {
                    monitor_runtime ().owns_async_command_processing.store (
                      false, std::memory_order_release);
                    return -1;
                }
                started_here = true;
            }
        }

        if (wait_for_quiescence) {
            wait_async_quiesced (10000);
            if (lifecycle.is_async_quiesce_pending ()) {
                errno = EBUSY;
                return -1;
            }
            continue;
        }
        if (started_here)
            lifecycle.wait_async_started (1000);
        return 0;
    }
}

void zlink::socket_base_t::release_monitor_async_command_processing (
  bool wait_for_quiescence_)
{
    {
        scoped_lock_t progress_lock (_transport_pair_owner_progress_sync);
        monitor_runtime_t &monitor = monitor_runtime ();
        if (!monitor.owns_async_command_processing.exchange (
              false, std::memory_order_acq_rel))
            return;
    }
    LIBZLINK_UNUSED (wait_for_quiescence_);
    request_unowned_async_command_processing_stop ();
}

void zlink::socket_base_t::request_unowned_async_command_processing_stop ()
{
    bool schedule_recheck = false;
    {
        scoped_lock_t progress_lock (_transport_pair_owner_progress_sync);
        const bool leased =
          monitor_runtime ().owns_async_command_processing.load (
            std::memory_order_acquire)
          || _transport_pair_owner_progress_refs != 0
          || _async_command_processing_retained.load (
            std::memory_order_acquire)
          || _completion_poller_refs.load (std::memory_order_acquire) != 0;
        if (leased || !lifecycle_coordinator ().is_async_mailbox_active ()) {
            _async_command_processing_stop_requested = false;
            return;
        }
        if (!_async_command_processing_stop_requested) {
            _async_command_processing_stop_requested = true;
            schedule_recheck = true;
        }
    }

    if (!schedule_recheck)
        return;

    // signal() alone does not post an idle mailbox handler. A real no-op
    // command makes the current executor revisit its idle detach boundary.
    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = this;
    wake.type = command_t::request_completion;
    static_cast<mailbox_t *> (_mailbox)->send (wake);
}

bool zlink::socket_base_t::stop_unowned_async_command_processing_at_idle ()
{
    bool stopped = false;
    {
        scoped_lock_t progress_lock (_transport_pair_owner_progress_sync);
        const bool leased =
          monitor_runtime ().owns_async_command_processing.load (
            std::memory_order_acquire)
          || _transport_pair_owner_progress_refs != 0
          || _async_command_processing_retained.load (
            std::memory_order_acquire)
          || _completion_poller_refs.load (std::memory_order_acquire) != 0;
        if (!_async_command_processing_stop_requested || leased
            || !lifecycle_coordinator ().is_async_mailbox_active ()) {
            if (leased)
                _async_command_processing_stop_requested = false;
            return false;
        }

#ifdef ZLINK_BUILD_TESTS
        invoke_async_owner_transition_test_hook (
          async_owner_test_idle_stop_gate_held);
#endif

        mailbox_t *const mailbox = static_cast<mailbox_t *> (_mailbox);
        // Keep the owner gate across the final empty check, physical detach and
        // lifecycle publication. An acquire therefore either cancels the
        // request before this point or observes a completely detached executor
        // and starts a new one; it can never borrow an executor already
        // committed to stop.
        if (!mailbox->detach_io_context_if_idle ())
            return false;
        invalidate_completion_processing_owner ();
        lifecycle_coordinator ().stop_async_mailbox_processing (NULL);
        _async_command_processing_stop_requested = false;
        // A new executor also takes the owner gate before installing itself, so
        // release the old receive lease before making the detached state
        // observable to that acquire.
        receive_runtime ().release_receive_sync_from_async_owner ();
        lifecycle_coordinator ().mark_async_processing_stopped (NULL);
        //  This executor consumed the primary notification descriptor while
        //  it drained the commands that led here (for example the
        //  activate_read of the first message after a monitor closed). The
        //  drain loop re-arms that descriptor only when it keeps running, so
        //  do it here as well, or a public poller sleeps through input this
        //  temporary owner already applied.
        mailbox->rearm_primary_signaler ();
        stopped = true;
    }
    if (stopped)
        notify_receive_progress ();
    return stopped;
}

int zlink::socket_base_t::acquire_transport_pair_owner_progress ()
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    for (;;) {
        bool wait_for_quiescence = false;
        {
            socket_public_api_scope_t admission (lifecycle);
            if (!admission.acquired ())
                return -1;

            socket_public_api_lock_scope_t guard (lifecycle);
            scoped_lock_t progress_lock (
              _transport_pair_owner_progress_sync);
            // Check the retiring executor before the reference shortcut. A
            // pre-existing lease does not make its mailbox owner usable while
            // an explicit completion-poller handoff is still detaching it.
            if (lifecycle.is_async_quiesce_pending ()) {
                wait_for_quiescence = true;
            } else if (_transport_pair_owner_progress_refs != 0) {
                ++_transport_pair_owner_progress_refs;
                _async_command_processing_stop_requested = false;
                return 0;
            } else {
                _transport_pair_owner_progress_refs = 1;
                _async_command_processing_stop_requested = false;
                if (lifecycle.is_async_mailbox_active ())
                    return 0;

                io_thread_t *io_thread = choose_io_thread (options.affinity);
                if (!io_thread) {
                    _transport_pair_owner_progress_refs = 0;
                    errno = EAGAIN;
                    return -1;
                }
                if (start_async_mailbox_processing (io_thread) != 0) {
                    _transport_pair_owner_progress_refs = 0;
                    return -1;
                }

                // Do not wait for the handler here. This method is called from
                // a session I/O callback and choose_io_thread() may select that
                // same thread. The mailbox is already scheduled and will run
                // after the callback returns.
                return 0;
            }
        }

        if (wait_for_quiescence) {
#ifdef ZLINK_BUILD_TESTS
            invoke_async_owner_transition_test_hook (
              async_owner_test_transport_acquire_waiting_for_quiesce);
#endif
            wait_async_quiesced (10000);
            if (lifecycle.is_async_quiesce_pending ()) {
                errno = EBUSY;
                return -1;
            }
        }
    }
}

void zlink::socket_base_t::release_transport_pair_owner_progress ()
{
    {
        scoped_lock_t progress_lock (_transport_pair_owner_progress_sync);
        if (_transport_pair_owner_progress_refs == 0)
            return;
        --_transport_pair_owner_progress_refs;
        if (_transport_pair_owner_progress_refs != 0)
            return;
    }
    request_unowned_async_command_processing_stop ();
}

void zlink::socket_base_t::process_stop ()
{
    //  Here, someone have called zlink_ctx_term while the socket was still alive.
    //  We'll remember the fact so that any blocking call is interrupted and any
    //  further attempt to use the socket will return ETERM. The user is still
    //  responsible for calling zlink_close on the socket though!
    _ctx_terminated = true;
    notify_receive_progress ();
    socket_completion::close (&completion_runtime (), ETERM);
    fail_all_send_pending (ETERM);

    scoped_lock_t lock (monitor_runtime ().sync);
    stop_monitor ();
}

void zlink::socket_base_t::process_bind (pipe_t *pipe_)
{
    // A termination acknowledgement can overtake bind during rapid endpoint
    // replacement. Do not add an already detached pipe back to the socket.
    if (pipe_->has_completed_termination ())
        return;
    attach_pipe (pipe_);
}

void zlink::socket_base_t::process_term (int linger_)
{
    //  Closing the socket ends every paired transport. Disable reconnect
    //  before either lane's pipe termination can reach session recovery and
    //  recreate an endpoint that the socket is already tearing down.
    endpoint_runtime ().disable_transport_pair_reconnects ();

    //  Unregister all inproc endpoints associated with this socket.
    //  Doing this we make sure that no new pipes from other sockets (inproc)
    //  will be initiated.
    unregister_endpoints (this);

    //  Ask all attached pipes to terminate.
    const size_t attached_pipe_count = endpoint_runtime ().attached_pipe_count ();
    int term_pipe_count = 0;
    for (size_t i = 0; i != attached_pipe_count; ++i) {
        //  Only inprocs might have a disconnect message set
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        if (!_term_pipes.insert (pipe).second)
            continue;
        pipe->send_disconnect_msg ();
        pipe->terminate (false);
        ++term_pipe_count;
    }
    register_term_acks (term_pipe_count);
    _term_pipe_acks_registered = term_pipe_count;
    _term_pipe_acks_received = 0;

    //  Continue the termination process immediately.
    own_t::process_term (linger_);
}

void zlink::socket_base_t::process_term_endpoint (std::string *endpoint_)
{
    term_endpoint_internal (endpoint_->c_str ());
    delete endpoint_;
}

void zlink::socket_base_t::set_all_pipes_nodelay ()
{
    for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i) {
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        if (pipe)
            pipe->set_nodelay ();
    }
}

void zlink::socket_base_t::refresh_attached_pipe_hwms ()
{
    scoped_lock_t lock (monitor_runtime ().sync);
    for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i) {
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        pipe->set_hwms (options.rcvhwm, options.sndhwm);
        pipe->send_hwms_to_peer (options.sndhwm, options.rcvhwm);
    }
}

void zlink::socket_base_t::update_pipe_options (int option_)
{
    if (option_ == ZLINK_INTERNAL_OPT_SNDHWM || option_ == ZLINK_INTERNAL_OPT_RCVHWM) {
        refresh_attached_pipe_hwms ();
    }
}

void zlink::socket_base_t::process_destroy ()
{
    lifecycle_coordinator ().mark_destroyed ();
}

void zlink::socket_base_t::in_event ()
{
    do {
        //  This function is invoked only once the socket is running in the
        //  context of the reaper thread. Process any commands from other
        //  threads/sockets that may be available at the moment. Ultimately,
        //  the socket will be destroyed.
        process_commands (0, false);
        if (lifecycle_coordinator ().is_destroyed ()) {
            check_destroy ();
            return;
        }
    } while (static_cast<mailbox_t *> (_mailbox)->reschedule_if_needed ());
}

void zlink::socket_base_t::process_async_mailbox ()
{
    lifecycle_coordinator ().mark_async_processing_started ();
    do {
        process_commands (0, false);
        drive_send_pending ();
        {
            // A public POLLCOMPLETION registration is the sole completion
            // owner while it exists. The gate also fences a 0 -> 1 owner
            // transfer against a drain that was already in flight.
            scoped_lock_t owner_lock (_completion_owner_sync);
            if (_completion_poller_refs.load (std::memory_order_acquire) == 0) {
                const completion_drain_scope_t drain_scope (this);
                acknowledge_request_completion_notification ();
                process_ready_completion_pipes ();
                (void) drain_request_completions ();
            }
        }
        if (lifecycle_coordinator ().is_destroyed ()) {
            if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
                mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
#ifdef ZLINK_BUILD_TESTS
                invoke_async_owner_transition_test_hook (
                  async_owner_test_explicit_stop_before_detach);
#endif
                if (!mailbox->detach_io_context_if_idle ())
                    continue;
                if (_completion_poller_refs.load (
                      std::memory_order_acquire)
                    == 0)
                    invalidate_completion_processing_owner ();
                receive_runtime ().release_receive_sync_from_async_owner ();
                lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
                mailbox->rearm_primary_signaler ();
                notify_receive_progress ();
            }
            check_destroy ();
            return;
        }
        if (stop_unowned_async_command_processing_at_idle ())
            return;
        if (lifecycle_coordinator ().is_async_mailbox_active ()) {
            process_deferred_socket_msg_pipe_terminations ();
            //  This executor consumed the mailbox's primary notification
            //  descriptor while draining commands. A poller that registered
            //  this socket watches that same descriptor, so re-arm it or the
            //  poller sleeps through the input this drain just applied.
            static_cast<mailbox_t *> (_mailbox)->rearm_primary_signaler ();
        }
        if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
            mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
#ifdef ZLINK_BUILD_TESTS
            invoke_async_owner_transition_test_hook (
              async_owner_test_explicit_stop_before_detach);
#endif
            if (!mailbox->detach_io_context_if_idle ())
                continue;
            if (_completion_poller_refs.load (std::memory_order_acquire) == 0)
                invalidate_completion_processing_owner ();
            //  Signal quiesce completion to waiting close()/start_reaping().
            receive_runtime ().release_receive_sync_from_async_owner ();
            lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
            mailbox->rearm_primary_signaler ();
            notify_receive_progress ();
            return;
        }
    } while (static_cast<mailbox_t *> (_mailbox)->reschedule_if_needed ());
}

void zlink::socket_base_t::notify_receive_progress ()
{
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t lock (receive.sync);
    notify_receive_progress_locked ();
}

void zlink::socket_base_t::notify_receive_progress_locked ()
{
    receive_runtime_t &receive = receive_runtime ();
    ++receive.progress_epoch;
    if (receive.waiters != 0)
        receive.progress_cv.broadcast ();
}

int zlink::socket_base_t::wait_receive_progress (uint64_t observed_epoch_,
                                                 int timeout_ms_)
{
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t lock (receive.sync);
    if (receive.progress_epoch == observed_epoch_) {
        ++receive.waiters;
#ifdef ZLINK_BUILD_TESTS
        if (receive.wait_hook)
            receive.wait_hook (receive.wait_hook_userdata);
#endif
        const int wait_rc = receive.progress_cv.wait (&receive.sync, timeout_ms_);
        zlink_assert (receive.waiters > 0);
        --receive.waiters;
        if (wait_rc != 0 && errno != EAGAIN)
            return -1;
    }

    if (_ctx_terminated) {
        errno = ETERM;
        return -1;
    }
    return 0;
}

void zlink::socket_base_t::out_event ()
{
    zlink_assert (false);
}

void zlink::socket_base_t::timer_event (int)
{
    zlink_assert (false);
}

void zlink::socket_base_t::check_destroy ()
{
    //  If the object was already marked as destroyed, finish the deallocation.
    if (lifecycle_coordinator ().is_destroyed ()) {
        lifecycle_coordinator ().mark_destroy_pending ();
        // Seal and observe the last lifetime reference in one atomic step.
        // Context snapshots may otherwise acquire a new pin after the zero
        // check but before the posted finalizer runs.
        if (!lifecycle_coordinator ().seal_mailbox_refs_if_zero ())
            return;

        if (_public_handle && !_public_handle->request_destroy ())
            return;

        schedule_finalize_destroy ();
    }
}

bool zlink::socket_base_t::try_inc_mailbox_ref ()
{
    return lifecycle_coordinator ().try_inc_mailbox_ref ();
}

void zlink::socket_base_t::inc_mailbox_ref ()
{
    lifecycle_coordinator ().inc_mailbox_ref ();
}

void zlink::socket_base_t::dec_mailbox_ref ()
{
    if (lifecycle_coordinator ().dec_mailbox_ref ()
        || !lifecycle_coordinator ().is_destroy_pending ())
        return;

    check_destroy ();
}

void zlink::socket_base_t::schedule_finalize_destroy ()
{
    zlink_assert (lifecycle_coordinator ().mailbox_refs_sealed ());
    zlink_assert (lifecycle_coordinator ().mailbox_refcount () == 0);
    const std::function<void ()> finalize = [this] () {
        this->finalize_destroy ();
    };
    if (lifecycle_coordinator ().reaper_poller ())
        boost::asio::post (lifecycle_coordinator ().reaper_poller ()->get_io_context (),
                           finalize);
    else
        finalize ();
}

void zlink::socket_base_t::finalize_destroy ()
{
    lifecycle_coordinator ().clear_destroy_pending ();

    //  Notify the reaper before removing the last socket from the context.
    //  destroy_socket() may ask the reaper to stop when the registry becomes
    //  empty; queuing the reaped notification first keeps the reaper's internal
    //  socket count ahead of that stop command.
    send_reaped ();

    //  Remove the socket from the context.
    destroy_socket (this);

    if (_public_handle)
        _public_handle->clear_socket ();

    _tag.store (0xdeadbeef, std::memory_order_release);

    //  Deallocate.
    own_t::process_destroy ();
}
