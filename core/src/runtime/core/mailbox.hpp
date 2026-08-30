/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_MAILBOX_HPP_INCLUDED__
#define __ZLINK_MAILBOX_HPP_INCLUDED__

#include <stddef.h>

#include "utils/config.hpp"
#include "core/command.hpp"
#include "core/ypipe.hpp"
#include "utils/mutex.hpp"
#include "core/i_mailbox.hpp"
#include "core/signaler.hpp"
#include "utils/fd.hpp"

#include <atomic>
#include <vector>

namespace boost
{
namespace asio
{
class io_context;
}
}

namespace zlink
{

class mailbox_t ZLINK_FINAL : public i_mailbox
{
  public:
    mailbox_t ();
    ~mailbox_t ();

    fd_t get_fd () const;
    void send (const command_t &cmd_);
    void signal ();
    int recv (command_t *cmd_, int timeout_);
    //  Returns true once for a command batch that woke an inactive receiver.
    //  Callers use this only as a fast-path hint; recv() remains authoritative.
    bool take_command_pending_hint ();
    //  A public poller may consume the primary notification while an async
    //  command owner applies the corresponding socket state. The command pipe
    //  remains authoritative even if that notification is consumed before
    //  the async owner enters recv().
    void drain_primary_signaler ();

    bool valid () const;

    typedef void (*mailbox_handler_t) (void *arg_);
    typedef void (*mailbox_pre_post_t) (void *arg_);
    void set_io_context (boost::asio::io_context *io_context_,
                         mailbox_handler_t handler_,
                         void *handler_arg_,
                         mailbox_pre_post_t pre_post_ = NULL);
    void schedule_if_needed ();
    void schedule_if_needed_unlocked ();
    bool reschedule_if_needed ();
    bool detach_io_context_if_idle ();

    // Signaler support for ZLINK_INTERNAL_OPT_FD
    void add_signaler (signaler_t *signaler_);
    void remove_signaler (signaler_t *signaler_);
    void signal_pollers ();
    //  Re-arm the primary notification descriptor after the async command
    //  executor has consumed it, so descriptor-based pollers watching this
    //  mailbox's fd still wake. See socket_base_t::process_async_mailbox.
    void rearm_primary_signaler ();
    void clear_signalers ();

#ifdef HAVE_FORK
    // close the file descriptors in the signaller. This is used in a forked
    // child process to close the file descriptors so that they do not interfere
    // with the context in the parent process.
    void forked () ZLINK_FINAL { _signaler.forked (); }
#endif

  private:
    //  The pipe to store actual commands.
    typedef ypipe_t<command_t, command_pipe_granularity> cpipe_t;
    cpipe_t _cpipe;

    //  Signaler to wake up a sleeping receiver.
    signaler_t _signaler;
    bool _active;

    //  There is an arbitrary number of threads sending. Given that ypipe
    //  requires a single writer, synchronise that endpoint. The receiving
    //  endpoint remains single-owner: socket runtimes serialize public/async
    //  handoff, while I/O, reaper and termination mailboxes each have one
    //  dedicated consumer.
    mutex_t _sync;

    boost::asio::io_context *_io_context;
    mailbox_handler_t _handler;
    void *_handler_arg;
    mailbox_pre_post_t _pre_post;
    std::atomic<bool> _scheduled;
    std::atomic<bool> _command_pending_hint;

    //  Signalers for ZLINK_INTERNAL_OPT_FD support
    std::vector<signaler_t *> _signalers;
    mutable std::atomic<bool> _primary_signaler_required;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (mailbox_t)
};
}

#endif
