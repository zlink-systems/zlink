/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <boost/asio.hpp>

#include "core/io_thread.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"

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

    static_cast<mailbox_t *> (_mailbox)->clear_signalers ();
    _tag = 0xdeadbeef;
    send_reap (this);
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

    //  Check whether there are any commands pending for this thread.
    command_t cmd;
    int rc = _mailbox->recv (&cmd, timeout_);

    if (rc != 0 && errno == EINTR)
        return -1;

    //  Process all available commands.
    while (rc == 0 || errno == EINTR) {
        if (rc == 0)
            cmd.destination->process_command (cmd);
        rc = _mailbox->recv (&cmd, 0);
    }

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

int zlink::socket_base_t::start_async_mailbox_processing (io_thread_t *io_thread_)
{
    return lifecycle_coordinator ().start_async_mailbox_processing (
      static_cast<mailbox_t *> (_mailbox), io_thread_, &socket_base_t::async_mailbox_handler, this,
      &socket_base_t::async_mailbox_pre_post);
}

void zlink::socket_base_t::stop_async_mailbox_processing ()
{
    lifecycle_coordinator ().stop_async_mailbox_processing (static_cast<mailbox_t *> (_mailbox));
}

void zlink::socket_base_t::wait_async_quiesced (int timeout_ms_)
{
    lifecycle_coordinator ().wait_async_quiesced (timeout_ms_);
}

void zlink::socket_base_t::process_stop ()
{
    //  Here, someone have called zlink_ctx_term while the socket was still alive.
    //  We'll remember the fact so that any blocking call is interrupted and any
    //  further attempt to use the socket will return ETERM. The user is still
    //  responsible for calling zlink_close on the socket though!
    scoped_lock_t lock (monitor_runtime ().sync);
    stop_monitor ();

    _ctx_terminated = true;
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
    //  This worker stands in for a completion poller when the application has
    //  not registered one, so it owns the completion drain while it runs.
    const completion_drain_scope_t drain_scope (this);
    do {
        process_commands (0, false);
        acknowledge_request_completion_notification ();
        process_ready_completion_pipes ();
        (void) drain_request_completion_controls ();
        if (lifecycle_coordinator ().is_destroyed ()) {
            if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
                mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
                if (!mailbox->detach_io_context_if_idle ())
                    continue;
                lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
                finish_deferred_close_after_async_quiesced ();
            }
            check_destroy ();
            return;
        }
        if (lifecycle_coordinator ().is_async_mailbox_active ())
            xdispatch_io ();
        if (!lifecycle_coordinator ().is_async_mailbox_active ()) {
            mailbox_t *mailbox = static_cast<mailbox_t *> (_mailbox);
            if (!mailbox->detach_io_context_if_idle ())
                continue;
            //  Signal quiesce completion to waiting close()/start_reaping().
            lifecycle_coordinator ().mark_async_processing_stopped (mailbox);
            finish_deferred_close_after_async_quiesced ();
            return;
        }
    } while (static_cast<mailbox_t *> (_mailbox)->reschedule_if_needed ());
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
        if (lifecycle_coordinator ().mailbox_refcount () != 0)
            return;

        inc_mailbox_ref ();
        if (lifecycle_coordinator ().reaper_poller ()) {
            boost::asio::post (lifecycle_coordinator ().reaper_poller ()->get_io_context (),
                               [this] () { this->dec_mailbox_ref (); });
        } else {
            dec_mailbox_ref ();
        }
    }
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

    finalize_destroy ();
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

    //  Deallocate.
    own_t::process_destroy ();
}
