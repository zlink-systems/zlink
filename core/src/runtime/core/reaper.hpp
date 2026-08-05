/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_REAPER_HPP_INCLUDED__
#define __ZLINK_REAPER_HPP_INCLUDED__

#include "core/object.hpp"
#include "core/mailbox.hpp"
#include "core/poller.hpp"
#include "core/i_poll_events.hpp"

namespace zlink
{
class ctx_t;
class socket_base_t;

class reaper_t ZLINK_FINAL : public object_t, public i_poll_events
{
  public:
    reaper_t (zlink::ctx_t *ctx_, uint32_t tid_);
    ~reaper_t ();

    mailbox_t *get_mailbox ();

    void start ();
    void stop ();

    //  i_poll_events implementation.
    void in_event ();
    void out_event ();
    void timer_event (int id_);

  private:
    //  Command handlers.
    void process_stop ();
    void process_reap (zlink::socket_base_t *socket_);
    void process_reaped ();
    void process_mailbox ();
    static void mailbox_handler (void *arg_);

    //  Reaper thread accesses incoming commands via this mailbox.
    mailbox_t _mailbox;

    //  I/O multiplexing is performed using a poller object.
    poller_t *_poller;

    //  Number of sockets being reaped at the moment.
    int _sockets;

    //  If true, we were already asked to terminate.
    bool _terminating;

    //  If true, the context was already notified that reaping is complete.
    bool _done_sent;

#ifdef HAVE_FORK
    // the process that created this context. Used to detect forking.
    pid_t _pid;
#endif

    ZLINK_NON_COPYABLE_NOR_MOVABLE (reaper_t)
};
}

#endif
