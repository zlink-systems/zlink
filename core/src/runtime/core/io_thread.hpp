/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_IO_THREAD_HPP_INCLUDED__
#define __ZLINK_IO_THREAD_HPP_INCLUDED__

#include "utils/stdint.hpp"
#include "core/object.hpp"
#include "core/poller.hpp"
#include "core/i_poll_events.hpp"
#include "core/mailbox.hpp"

#include <boost/asio.hpp>

namespace zlink
{
class ctx_t;

//  Generic part of the I/O thread. Polling-mechanism-specific features
//  are implemented in separate "polling objects".

class io_thread_t ZLINK_FINAL : public object_t, public i_poll_events
{
  public:
    io_thread_t (zlink::ctx_t *ctx_, uint32_t tid_);

    //  Clean-up. If the thread was started, it's necessary to call 'stop'
    //  before invoking destructor. Otherwise the destructor would hang up.
    ~io_thread_t ();

    //  Launch the physical thread.
    void start ();

    //  Ask underlying thread to stop.
    void stop ();

    //  Returns mailbox associated with this I/O thread.
    mailbox_t *get_mailbox ();

    //  i_poll_events implementation.
    void in_event ();
    void out_event ();
    void timer_event (int id_);

    //  Used by io_objects to retrieve the associated poller object.
    poller_t *get_poller () const;

    //  Get access to the io_context for ASIO-based operations
    boost::asio::io_context &get_io_context () const;

    //  Command handlers.
    void process_stop ();

    //  Returns load experienced by the I/O thread.
    int get_load () const;

  private:
    //  I/O thread accesses incoming commands via this mailbox.
    mailbox_t _mailbox;

    //  I/O multiplexing is performed using a poller object.
    poller_t *_poller;

    static void mailbox_handler (void *arg_);
    void process_mailbox ();

    ZLINK_NON_COPYABLE_NOR_MOVABLE (io_thread_t)
};
}

#endif
