/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <stddef.h>
#include "core/poller.hpp"
#include "sockets/proxy/proxy.hpp"
#include "utils/likely.hpp"
#include "core/msg.hpp"

#if defined ZLINK_POLL_BASED_ON_POLL && !defined ZLINK_HAVE_WINDOWS && !defined ZLINK_HAVE_AIX
#include <poll.h>
#endif

// These headers end up pulling in zlink.h somewhere in their include
// dependency chain
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

#ifdef ZLINK_HAVE_POLLER

#include "core/socket_poller.hpp"

//  Macros for repetitive code.

//  PROXY_CLEANUP() must not be used before these variables are initialized.
#define PROXY_CLEANUP()                                                                            \
    do {                                                                                           \
        delete poller_all;                                                                         \
        delete poller_in;                                                                          \
        delete poller_receive_blocked;                                                             \
        delete poller_send_blocked;                                                                \
        delete poller_both_blocked;                                                                \
        delete poller_frontend_only;                                                               \
        delete poller_backend_only;                                                                \
    } while (false)


#define CHECK_RC_EXIT_ON_FAILURE()                                                                 \
    do {                                                                                           \
        if (rc < 0) {                                                                              \
            PROXY_CLEANUP ();                                                                      \
            return close_and_return (&msg, -1);                                                    \
        }                                                                                          \
    } while (false)

#endif //  ZLINK_HAVE_POLLER

static int capture (class zlink::socket_base_t *capture_, zlink::msg_t *msg_, int more_ = 0)
{
    //  Copy message to capture socket if any
    if (capture_) {
        zlink::msg_t ctrl;
        int rc = ctrl.init ();
        if (unlikely (rc < 0))
            return -1;
        rc = ctrl.copy (*msg_);
        if (unlikely (rc < 0))
            return -1;
        rc = capture_->send (&ctrl, more_ ? ZLINK_SNDMORE : 0);
        if (unlikely (rc < 0))
            return -1;
    }
    return 0;
}

static int forward (class zlink::socket_base_t *from_,
                    class zlink::socket_base_t *to_,
                    class zlink::socket_base_t *capture_,
                    zlink::msg_t *msg_)
{
    // Forward a burst of messages
    for (unsigned int i = 0; i < zlink::proxy_burst_size; i++) {
        // Forward all the parts of one message
        while (true) {
            int rc = from_->recv (msg_, ZLINK_DONTWAIT);
            if (rc < 0) {
                if (likely (errno == EAGAIN && i > 0))
                    return 0; // End of burst

                return -1;
            }

            const bool more = (msg_->flags () & zlink::msg_t::more) != 0;

            //  Copy message to capture socket if any
            rc = capture (capture_, msg_, more);
            if (unlikely (rc < 0))
                return -1;

            rc = to_->send (msg_, more ? ZLINK_SNDMORE : 0);
            if (unlikely (rc < 0))
                return -1;
            if (more == 0)
                break;
        }
    }

    return 0;
}

#ifdef ZLINK_HAVE_POLLER
int zlink::proxy (class socket_base_t *frontend_,
                  class socket_base_t *backend_,
                  class socket_base_t *capture_)
{
    msg_t msg;
    int rc = msg.init ();
    if (rc != 0)
        return -1;

    //  The algorithm below assumes ratio of requests and replies processed
    //  under full load to be 1:1.

    bool frontend_equal_to_backend;
    bool frontend_in = false;
    bool frontend_out = false;
    bool backend_in = false;
    bool backend_out = false;
    zlink::socket_poller_t::event_t events[3];
    const int nevents = 2;

    //  Don't allocate these pollers from stack because they will take more than 900 kB of stack!
    //  On Windows this blows up default stack of 1 MB and aborts the program.
    //  I wanted to use std::shared_ptr here as the best solution but that requires C++11...
    zlink::socket_poller_t *poller_all =
      new (std::nothrow) zlink::socket_poller_t; //  Poll for everything.
    zlink::socket_poller_t *poller_in = new (std::nothrow) zlink::
      socket_poller_t; //  Poll only 'ZLINK_POLLIN' on all sockets. Initial blocking poll in loop.
    zlink::socket_poller_t *poller_receive_blocked =
      new (std::nothrow) zlink::socket_poller_t; //  All except 'ZLINK_POLLIN' on 'frontend_'.

    //  If frontend_==backend_ 'poller_send_blocked' and 'poller_receive_blocked' are the same, 'ZLINK_POLLIN' is ignored.
    //  In that case 'poller_send_blocked' is not used. We need only 'poller_receive_blocked'.
    //  We also don't need 'poller_both_blocked', 'poller_backend_only' nor 'poller_frontend_only' no need to initialize it.
    //  We save some RAM and time for initialization.
    zlink::socket_poller_t *poller_send_blocked = NULL; //  All except 'ZLINK_POLLIN' on 'backend_'.
    zlink::socket_poller_t *poller_both_blocked =
      NULL; //  All except 'ZLINK_POLLIN' on both 'frontend_' and 'backend_'.
    zlink::socket_poller_t *poller_frontend_only =
      NULL; //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'frontend_'.
    zlink::socket_poller_t *poller_backend_only =
      NULL; //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'backend_'.

    if (frontend_ != backend_) {
        poller_send_blocked =
          new (std::nothrow) zlink::socket_poller_t; //  All except 'ZLINK_POLLIN' on 'backend_'.
        poller_both_blocked = new (std::nothrow)
          zlink::socket_poller_t; //  All except 'ZLINK_POLLIN' on both 'frontend_' and 'backend_'.
        poller_frontend_only = new (std::nothrow)
          zlink::socket_poller_t; //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'frontend_'.
        poller_backend_only = new (std::nothrow)
          zlink::socket_poller_t; //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'backend_'.
        frontend_equal_to_backend = false;
    } else
        frontend_equal_to_backend = true;

    if (poller_all == NULL || poller_in == NULL || poller_receive_blocked == NULL
        || ((poller_send_blocked == NULL || poller_both_blocked == NULL)
            && !frontend_equal_to_backend)) {
        PROXY_CLEANUP ();
        return close_and_return (&msg, -1);
    }

    zlink::socket_poller_t *poller_wait =
      poller_in; //  Poller for blocking wait, initially all 'ZLINK_POLLIN'.

    //  Register 'frontend_' and 'backend_' with pollers.
    rc = poller_all->add (frontend_, NULL,
                          ZLINK_POLLIN | ZLINK_POLLOUT); //  Everything.
    CHECK_RC_EXIT_ON_FAILURE ();
    rc = poller_in->add (frontend_, NULL, ZLINK_POLLIN); //  All 'ZLINK_POLLIN's.
    CHECK_RC_EXIT_ON_FAILURE ();

    if (frontend_equal_to_backend) {
        //  If frontend_==backend_ 'poller_send_blocked' and 'poller_receive_blocked' are the same,
        //  so we don't need 'poller_send_blocked'. We need only 'poller_receive_blocked'.
        //  We also don't need 'poller_both_blocked', no need to initialize it.
        rc = poller_receive_blocked->add (frontend_, NULL, ZLINK_POLLOUT);
        CHECK_RC_EXIT_ON_FAILURE ();
    } else {
        rc = poller_all->add (backend_, NULL,
                              ZLINK_POLLIN | ZLINK_POLLOUT); //  Everything.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_in->add (backend_, NULL, ZLINK_POLLIN); //  All 'ZLINK_POLLIN's.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_both_blocked->add (frontend_, NULL,
                                       ZLINK_POLLOUT); //  Waiting only for 'ZLINK_POLLOUT'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_both_blocked->add (backend_, NULL,
                                       ZLINK_POLLOUT); //  Waiting only for 'ZLINK_POLLOUT'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_send_blocked->add (backend_, NULL,
                                       ZLINK_POLLOUT); //  All except 'ZLINK_POLLIN' on 'backend_'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_send_blocked->add (
          frontend_, NULL,
          ZLINK_POLLIN | ZLINK_POLLOUT); //  All except 'ZLINK_POLLIN' on 'backend_'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc =
          poller_receive_blocked->add (frontend_, NULL,
                                       ZLINK_POLLOUT); //  All except 'ZLINK_POLLIN' on 'frontend_'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_receive_blocked->add (
          backend_, NULL,
          ZLINK_POLLIN | ZLINK_POLLOUT); //  All except 'ZLINK_POLLIN' on 'frontend_'.
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_frontend_only->add (frontend_, NULL, ZLINK_POLLIN | ZLINK_POLLOUT);
        CHECK_RC_EXIT_ON_FAILURE ();
        rc = poller_backend_only->add (backend_, NULL, ZLINK_POLLIN | ZLINK_POLLOUT);
        CHECK_RC_EXIT_ON_FAILURE ();
    }

    bool request_processed = false, reply_processed = false;

    while (true) {
        //  Blocking wait initially only for 'ZLINK_POLLIN' - 'poller_wait' points to 'poller_in'.
        //  If one of receiving end's queue is full ('ZLINK_POLLOUT' not available),
        //  'poller_wait' is pointed to 'poller_receive_blocked', 'poller_send_blocked' or 'poller_both_blocked'.
        rc = poller_wait->wait (events, nevents, -1);
        if (rc < 0 && errno == EAGAIN)
            rc = 0;
        CHECK_RC_EXIT_ON_FAILURE ();

        //  Some of events waited for by 'poller_wait' have arrived, now poll for everything without blocking.
        rc = poller_all->wait (events, nevents, 0);
        if (rc < 0 && errno == EAGAIN)
            rc = 0;
        CHECK_RC_EXIT_ON_FAILURE ();

        //  Process events.
        for (int i = 0; i < rc; i++) {
            if (events[i].socket == frontend_) {
                frontend_in = (events[i].events & ZLINK_POLLIN) != 0;
                frontend_out = (events[i].events & ZLINK_POLLOUT) != 0;
            } else
                //  This 'if' needs to be after check for 'frontend_' in order never
                //  to be reached in case frontend_==backend_, so we ensure backend_in=false in that case.
                if (events[i].socket == backend_) {
                    backend_in = (events[i].events & ZLINK_POLLIN) != 0;
                    backend_out = (events[i].events & ZLINK_POLLOUT) != 0;
                }
        }

        //  Process a request, 'ZLINK_POLLIN' on 'frontend_' and 'ZLINK_POLLOUT' on 'backend_'.
        //  In case of frontend_==backend_ there's no 'ZLINK_POLLOUT' event.
        if (frontend_in && (backend_out || frontend_equal_to_backend)) {
            rc = forward (frontend_, backend_, capture_, &msg);
            CHECK_RC_EXIT_ON_FAILURE ();
            request_processed = true;
            frontend_in = backend_out = false;
        } else
            request_processed = false;

        //  Process a reply, 'ZLINK_POLLIN' on 'backend_' and 'ZLINK_POLLOUT' on 'frontend_'.
        //  If 'frontend_' and 'backend_' are the same this is not needed because previous processing
        //  covers all of the cases. 'backend_in' is always false if frontend_==backend_ due to
        //  design in 'for' event processing loop.
        if (backend_in && frontend_out) {
            rc = forward (backend_, frontend_, capture_, &msg);
            CHECK_RC_EXIT_ON_FAILURE ();
            reply_processed = true;
            backend_in = frontend_out = false;
        } else
            reply_processed = false;

        if (request_processed || reply_processed) {
                //  If request/reply is processed that means we had at least one 'ZLINK_POLLOUT' event.
                //  Enable corresponding 'ZLINK_POLLIN' for blocking wait if any was disabled.
                if (poller_wait != poller_in) {
                    if (request_processed) { //  'frontend_' -> 'backend_'
                        if (poller_wait == poller_both_blocked)
                            poller_wait = poller_send_blocked;
                        else if (poller_wait == poller_receive_blocked
                                 || poller_wait == poller_frontend_only)
                            poller_wait = poller_in;
                    }
                    if (reply_processed) { //  'backend_' -> 'frontend_'
                        if (poller_wait == poller_both_blocked)
                            poller_wait = poller_receive_blocked;
                        else if (poller_wait == poller_send_blocked
                                 || poller_wait == poller_backend_only)
                            poller_wait = poller_in;
                    }
                }
        } else {
                //  No requests have been processed, there were no 'ZLINK_POLLIN' with corresponding 'ZLINK_POLLOUT' events.
                //  That means that out queue(s) is/are full or one out queue is full and second one has no messages to process.
                //  Disable receiving 'ZLINK_POLLIN' for sockets for which there's no 'ZLINK_POLLOUT',
                //  or wait only on both 'backend_''s or 'frontend_''s 'ZLINK_POLLIN' and 'ZLINK_POLLOUT'.
                if (frontend_in) {
                    if (frontend_out)
                        // If frontend_in and frontend_out are true, obviously backend_in and backend_out are both false.
                        // In that case we need to wait for both 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' only on 'backend_'.
                        // We'll never get here in case of frontend_==backend_ because then frontend_out will always be false.
                        poller_wait = poller_backend_only;
                    else {
                        if (poller_wait == poller_send_blocked)
                            poller_wait = poller_both_blocked;
                        else if (poller_wait == poller_in)
                            poller_wait = poller_receive_blocked;
                    }
                }
                if (backend_in) {
                    //  Will never be reached if frontend_==backend_, 'backend_in' will
                    //  always be false due to design in 'for' event processing loop.
                    if (backend_out)
                        // If backend_in and backend_out are true, obviously frontend_in and frontend_out are both false.
                        // In that case we need to wait for both 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' only on 'frontend_'.
                        poller_wait = poller_frontend_only;
                    else {
                        if (poller_wait == poller_receive_blocked)
                            poller_wait = poller_both_blocked;
                        else if (poller_wait == poller_in)
                            poller_wait = poller_send_blocked;
                    }
                }
        }
    }
}

#else //  ZLINK_HAVE_POLLER

int zlink::proxy (class socket_base_t *frontend_,
                  class socket_base_t *backend_,
                  class socket_base_t *capture_)
{
    msg_t msg;
    int rc = msg.init ();
    if (rc != 0)
        return -1;

    //  The algorithm below assumes ratio of requests and replies processed
    //  under full load to be 1:1.

    zlink_pollitem_t items[] = {{frontend_, 0, ZLINK_POLLIN, 0},
                                {backend_, 0, ZLINK_POLLIN, 0}};

    zlink_pollitem_t itemsout[] = {{frontend_, 0, ZLINK_POLLOUT, 0},
                                   {backend_, 0, ZLINK_POLLOUT, 0}};

    while (true) {
        //  Wait while there are either requests or replies to process.
        rc = zlink_poll (&items[0], 2, -1, NULL);
        if (unlikely (rc < 0))
            return close_and_return (&msg, -1);

        //  Get the pollout separately because when combining this with pollin it maxes the CPU
        //  because pollout shall most of the time return directly.
        //  POLLOUT is only checked when frontend and backend sockets are not the same.
        if (frontend_ != backend_) {
            rc = zlink_poll (&itemsout[0], 2, 0, NULL);
            if (unlikely (rc < 0)) {
                return close_and_return (&msg, -1);
            }
        }

        if (items[0].revents & ZLINK_POLLIN
            && (frontend_ == backend_ || itemsout[1].revents & ZLINK_POLLOUT)) {
            rc = forward (frontend_, backend_, capture_, &msg);
            if (unlikely (rc < 0))
                return close_and_return (&msg, -1);
        }
        //  Process a reply
        if (frontend_ != backend_ && items[1].revents & ZLINK_POLLIN
            && itemsout[0].revents & ZLINK_POLLOUT) {
            rc = forward (backend_, frontend_, capture_, &msg);
            if (unlikely (rc < 0))
                return close_and_return (&msg, -1);
        }
    }
}

#endif //  ZLINK_HAVE_POLLER
