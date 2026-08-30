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
}

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

void zlink::socket_base_t::defer_close_handoff_from_async_owner ()
{
    if (lifecycle_coordinator ().is_async_mailbox_active ())
        stop_async_mailbox_processing ();
}

void zlink::socket_base_t::finish_deferred_close_after_async_quiesced ()
{
    if (!lifecycle_coordinator ().take_deferred_close ())
        return;

    finish_close_reap ();
}

void zlink::socket_base_t::start_reaping (poller_t *poller_)
{
    //  The mailbox must have exactly one executor owner. A close initiated by
    //  its own callback cannot wait for that callback here, so the reaper is
    //  the ownership boundary that waits until the old executor has detached.
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

int zlink::socket_base_t::process_commands (int timeout_, bool throttle_)
{
    receive_runtime_t &receive = receive_runtime ();
    const bool async_executor = current_async_mailbox_dispatch_socket () == this;
    // Once the mailbox handoff begins, the async executor is the sole command
    // progress owner.  A public socket path may already hold a socket-specific
    // API mutex, so it must not wait for the async executor while that executor
    // can re-enter the same socket during command dispatch.
    if (async_mailbox_owns_commands () && !async_executor) {
        if (_ctx_terminated) {
            errno = ETERM;
            return -1;
        }
        return 0;
    }

    scoped_lock_t command_owner (receive.command_owner_sync);
    // Close the handoff race between the lock-free owner check above and
    // acquiring the command scope.  The pending handoff bit remains set until
    // the async owner has been installed.
    if (async_mailbox_owns_commands () && !async_executor) {
        if (_ctx_terminated) {
            errno = ETERM;
            return -1;
        }
        return 0;
    }

    if (timeout_ == 0) {
        //  If we are asked not to wait, check whether we haven't processed
        //  commands recently, so that we can throttle the new commands.

        //  Get the CPU's tick counter. If 0, the counter is not available.
        const uint64_t tsc = zlink::clock_t::rdtsc ();

        //  Optimised version of command processing - it doesn't have to check
        //  for incoming commands each time. It does so only if certain time
        //  elapsed since last command processing. Command delay varies
        //  depending on CPU speed: It's ~1ms on 3GHz CPU, ~2ms on 1.5GHz CPU
        //  etc. The optimisation makes sense only on platforms where getting
        //  a timestamp is a very cheap operation (tens of nanoseconds).
        if (tsc && throttle_ && command_runtime ().should_skip_throttled_command_poll (tsc))
            return 0;
    }

#ifdef ZLINK_BUILD_TESTS
    if (async_executor)
        receive.async_mailbox_drains.fetch_add (1, std::memory_order_relaxed);
    else
        receive.public_mailbox_drains.fetch_add (1, std::memory_order_relaxed);
#endif

    //  Check whether there are any commands pending for this thread.
    command_t cmd;
    int rc = _mailbox->recv (&cmd, timeout_);

    if (rc != 0 && errno == EINTR)
        return -1;

    //  Process all available commands.
    bool processed_command = false;
    while (rc == 0 || errno == EINTR) {
        if (rc == 0) {
            // Pipe commands mutate inbound state before reaching the socket
            // sink. When the mailbox runs on an I/O thread, it can also attach
            // or detach a pipe while a public send uses socket-specific
            // distribution state (for example DEALER's load balancer).
            // Serialize socket-owned send-state mutation with active and
            // close-time multipart rollback. Non-PAIR send state is touched by
            // several command types and keeps the existing broad guard. PAIR
            // owns only one raw pipe pointer: bind publishes it and a
            // termination acknowledgement clears/deallocates it. Guard those
            // two mutation commands unconditionally rather than checking a
            // multipart marker, which would leave a check-then-admit race. The
            // frequent PAIR activation/pending commands remain lock-free.
            const bool pair_pipe_lifetime_command =
              options.type == ZLINK_CORE_SOCKET_PAIR
              && (cmd.type == command_t::bind
                  || cmd.type == command_t::pipe_term_ack);
#ifdef ZLINK_BUILD_TESTS
            receive_runtime_t::command_sync_probe_hook_fn sync_probe =
              receive.command_sync_probe_hook.load (std::memory_order_acquire);
            if (sync_probe) {
                const bool acquired = receive.sync.try_lock ();
                if (acquired)
                    receive.sync.unlock ();
                sync_probe (
                  receive.command_sync_probe_userdata.load (
                    std::memory_order_acquire),
                  static_cast<int> (cmd.type),
                  !acquired);
            }
#endif
            if (async_executor
                && (direct_send_needs_public_api_sync ()
                    || pair_pipe_lifetime_command)) {
                socket_public_api_lock_scope_t api_owner (
                  lifecycle_coordinator ());
                scoped_lock_t receive_owner (receive.sync);
                cmd.destination->process_command (cmd);
            } else {
                scoped_lock_t receive_owner (receive.sync);
                cmd.destination->process_command (cmd);
            }
            // Pipe termination callbacks defer socket-message assembly/routing
            // cleanup because the command itself runs under receive.sync. The
            // local scope above is gone here, so waiting for a direct handler
            // cannot invert receive and dispatch locks.
            process_deferred_socket_msg_pipe_terminations ();
            processed_command = true;
        }
        rc = _mailbox->recv (&cmd, 0);
    }

    // A public socket poller owns a secondary signaler. Publish readiness only
    // after the async command owner has applied the state transition so the
    // poller cannot consume the command wake before has_in()/has_out() changes.
    if (processed_command)
        static_cast<mailbox_t *> (_mailbox)->signal_pollers ();

    zlink_assert (errno == EAGAIN);

    if (_ctx_terminated) {
        errno = ETERM;
        return -1;
    }

    return 0;
}

int zlink::socket_base_t::process_submit_commands ()
{
    return process_commands (0, true);
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
    // executor can enter xdispatch_io()/command delivery under receive.sync.
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
        scoped_lock_t command_owner (receive.command_owner_sync);
        lifecycle_coordinator ().stop_async_mailbox_processing (
          static_cast<mailbox_t *> (_mailbox));
    }
    notify_receive_progress ();
    static_cast<mailbox_t *> (_mailbox)->signal ();
}

void zlink::socket_base_t::wait_async_quiesced (int timeout_ms_)
{
    lifecycle_coordinator ().wait_async_quiesced (timeout_ms_);
}

void zlink::socket_base_t::retain_async_command_processing ()
{
    monitor_runtime ().owns_async_command_processing.store (
      false, std::memory_order_release);
}

int zlink::socket_base_t::ensure_async_command_processing ()
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    socket_public_api_lock_scope_t guard (lifecycle);
    if (lifecycle.is_async_mailbox_active ())
        return 0;

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EAGAIN;
        return -1;
    }
    if (start_async_mailbox_processing (io_thread) != 0)
        return -1;
    lifecycle.wait_async_started (1000);
    return 0;
}

void zlink::socket_base_t::process_stop ()
{
    //  Here, someone have called zlink_ctx_term while the socket was still alive.
    //  We'll remember the fact so that any blocking call is interrupted and any
    //  further attempt to use the socket will return ETERM. The user is still
    //  responsible for calling zlink_close on the socket though!
    _ctx_terminated = true;
    notify_receive_progress ();
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
        bool completion_poller_owned = false;
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
                //  Send completions share the reply completion's dispatch
                //  owner. A POLLCOMPLETION registration moves both at once,
                //  which is what makes "same callback, different dispatch
                //  location" true rather than a second channel.
                dispatch_send_completions (false);
            } else
                completion_poller_owned = true;
        }
        // The mailbox worker can win admission after the poller consumed the
        // command wake and skipped the still-owned admission gate. Re-enter
        // the common handoff only for the poller-owned branch: it checks the
        // actual completion queue under the owner gate and publishes a fresh
        // wake, or dispatches locally if ownership changed in between.
        if (completion_poller_owned)
            dispatch_send_completions_if_local ();
        if (lifecycle_coordinator ().is_destroyed ()) {
            if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
                mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
                if (!mailbox->detach_io_context_if_idle ())
                    continue;
                lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
                receive_runtime ().release_receive_sync_from_async_owner ();
                notify_receive_progress ();
                finish_deferred_close_after_async_quiesced ();
            }
            check_destroy ();
            return;
        }
        if (lifecycle_coordinator ().is_async_mailbox_active ()) {
            if (socket_msg_dispatch_active ()) {
                // Raw socket-message xread_activated() publishes dispatch
                // work while it owns receive-side state. Drain only after that
                // ownership is gone; its callback may re-enter recv/send.
                defer_socket_msg_dispatch ();
                process_deferred_socket_msg_pipe_terminations ();
            } else {
                // SUB/XPUB and the remaining established async consumers own
                // their socket-specific xdispatch_io semantics independently
                // of the raw socket-message bridge.
                scoped_lock_t receive_lock (receive_runtime ().sync);
                xdispatch_io ();
            }
            //  This executor consumed the mailbox's primary notification
            //  descriptor while draining commands. A poller that registered
            //  this socket watches that same descriptor, so re-arm it or the
            //  poller sleeps through the input this drain just applied.
            static_cast<mailbox_t *> (_mailbox)->rearm_primary_signaler ();
        }
        if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
            mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
            if (!mailbox->detach_io_context_if_idle ())
                continue;
            //  Signal quiesce completion to waiting close()/start_reaping().
            lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
            receive_runtime ().release_receive_sync_from_async_owner ();
            notify_receive_progress ();
            finish_deferred_close_after_async_quiesced ();
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
