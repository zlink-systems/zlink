/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_IO_OBJECT_HPP_INCLUDED__
#define __ZLINK_IO_OBJECT_HPP_INCLUDED__

#include <stddef.h>

#include "utils/stdint.hpp"
#include "utils/fd.hpp"
#include "core/poller.hpp"
#include "core/i_poll_events.hpp"

namespace zlink
{
class io_thread_t;

//  Simple base class for objects that live in I/O threads.
//  It makes communication with the poller object easier and
//  makes defining unneeded event handlers unnecessary.

class io_object_t : public i_poll_events
{
  public:
    io_object_t (zlink::io_thread_t *io_thread_ = NULL);
    ~io_object_t () ZLINK_OVERRIDE;

    //  When migrating an object from one I/O thread to another, first
    //  unplug it, then migrate it, then plug it to the new thread.
    void plug (zlink::io_thread_t *io_thread_);
    void unplug ();

  protected:
    typedef poller_t::handle_t handle_t;

    //  Methods to access underlying poller object.
    handle_t add_fd (fd_t fd_);
    void rm_fd (handle_t handle_);
    void set_pollin (handle_t handle_);
    void reset_pollin (handle_t handle_);
    void set_pollout (handle_t handle_);
    void reset_pollout (handle_t handle_);
    void add_timer (int timeout_, int id_);
    void cancel_timer (int id_);

    //  i_poll_events interface implementation.
    void in_event () ZLINK_OVERRIDE;
    void out_event () ZLINK_OVERRIDE;
    void timer_event (int id_) ZLINK_OVERRIDE;

  private:
    poller_t *_poller;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (io_object_t)
};
}

#endif
