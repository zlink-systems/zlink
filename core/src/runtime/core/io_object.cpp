/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/io_object.hpp"
#include "core/io_thread.hpp"
#include "utils/err.hpp"

zlink::io_object_t::io_object_t (io_thread_t *io_thread_) : _poller (NULL)
{
    if (io_thread_)
        plug (io_thread_);
}

zlink::io_object_t::~io_object_t ()
{
}

void zlink::io_object_t::plug (io_thread_t *io_thread_)
{
    zlink_assert (io_thread_);
    zlink_assert (!_poller);

    //  Retrieve the poller from the thread we are running in.
    _poller = io_thread_->get_poller ();
}

void zlink::io_object_t::unplug ()
{
    zlink_assert (_poller);

    //  Forget about old poller in preparation to be migrated
    //  to a different I/O thread.
    _poller = NULL;
}

zlink::io_object_t::handle_t zlink::io_object_t::add_fd (fd_t fd_)
{
    return _poller->add_fd (fd_, this);
}

void zlink::io_object_t::rm_fd (handle_t handle_)
{
    _poller->rm_fd (handle_);
}

void zlink::io_object_t::set_pollin (handle_t handle_)
{
    _poller->set_pollin (handle_);
}

void zlink::io_object_t::reset_pollin (handle_t handle_)
{
    _poller->reset_pollin (handle_);
}

void zlink::io_object_t::set_pollout (handle_t handle_)
{
    _poller->set_pollout (handle_);
}

void zlink::io_object_t::reset_pollout (handle_t handle_)
{
    _poller->reset_pollout (handle_);
}

void zlink::io_object_t::add_timer (int timeout_, int id_)
{
    _poller->add_timer (timeout_, this, id_);
}

void zlink::io_object_t::cancel_timer (int id_)
{
    _poller->cancel_timer (this, id_);
}

void zlink::io_object_t::in_event ()
{
    zlink_assert (false);
}

void zlink::io_object_t::out_event ()
{
    zlink_assert (false);
}

void zlink::io_object_t::timer_event (int)
{
    zlink_assert (false);
}
