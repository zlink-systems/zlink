/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <stddef.h>
#include "core/socket_poller.hpp"
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

#ifdef ZLINK_BUILD_TESTS
namespace
{
std::atomic<bool> g_fail_next_proxy_destination_send (false);
}

void zlink::test_fail_next_proxy_destination_send ()
{
    g_fail_next_proxy_destination_send.store (true,
                                               std::memory_order_release);
}

void zlink::test_reset_proxy_state ()
{
    g_fail_next_proxy_destination_send.store (false,
                                               std::memory_order_release);
}
#endif

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

static int capture (class zlink::socket_base_t *capture_, zlink::msg_t *msg_, int more_ = 0)
{
    //  Copy message to capture socket if any
    if (capture_) {
        zlink::msg_t ctrl;
        int rc = ctrl.init ();
        if (unlikely (rc < 0))
            return -1;
        rc = ctrl.copy (*msg_);
        if (unlikely (rc < 0)) {
            const int saved_errno = errno;
            (void) ctrl.close ();
            errno = saved_errno;
            return -1;
        }
        rc = capture_->send (&ctrl, more_ ? ZLINK_SNDMORE : 0);
        const int saved_errno = errno;
        (void) ctrl.close ();
        errno = saved_errno;
        if (unlikely (rc < 0))
            return -1;
    }
    return 0;
}

class proxy_source_pipe_pin_t
{
  public:
    explicit proxy_source_pipe_pin_t (zlink::pipe_t *pipe_) : _pipe (pipe_) {}
    ~proxy_source_pipe_pin_t ()
    {
        if (_pipe)
            _pipe->release_lifetime_ref ();
    }

    zlink::pipe_t *get () const { return _pipe; }

  private:
    zlink::pipe_t *_pipe;
};

static void reset_proxy_receive_message (zlink::msg_t *msg_)
{
    if (msg_->check ()) {
        const int close_rc = msg_->close ();
        errno_assert (close_rc == 0);
    }
    const int init_rc = msg_->init ();
    errno_assert (init_rc == 0);
}

static void rollback_proxy_record (zlink::socket_base_t *to_,
                                   zlink::socket_base_t *capture_,
                                   zlink::msg_t *msg_)
{
    // Both destinations are owned by this proxy loop. rollback() is a no-op
    // when a destination has not accepted a MORE frame, so use one cleanup for
    // every failed exit rather than trying to reconstruct which of capture/to
    // won the most recent write.
    (void) to_->rollback ();
    if (capture_)
        (void) capture_->rollback ();
    reset_proxy_receive_message (msg_);
}

static void reject_proxy_record (zlink::socket_base_t *from_,
                                 zlink::socket_base_t *to_,
                                 zlink::socket_base_t *capture_,
                                 zlink::msg_t *msg_,
                                 zlink::pipe_t *source_pipe_,
                                 bool current_has_more_)
{
    bool more = current_has_more_;
    while (more) {
        zlink::pipe_t *tail_pipe = NULL;
        const int recv_rc = from_->recv_pipe (
          msg_, &tail_pipe, ZLINK_DONTWAIT, true);
        proxy_source_pipe_pin_t tail_pin (tail_pipe);
        if (recv_rc != 0)
            break;
        more = (msg_->flags () & zlink::msg_t::more) != 0;
    }

    // Earlier MORE frames are not visible until the record commits. Remove
    // them from both output transactions before terminating the bad source.
    rollback_proxy_record (to_, capture_, msg_);
    if (source_pipe_)
        source_pipe_->terminate (false);
}

static int forward (class zlink::socket_base_t *from_,
                    class zlink::socket_base_t *to_,
                    class zlink::socket_base_t *capture_,
                    zlink::msg_t *msg_)
{
    // Forward a burst of messages
    for (unsigned int i = 0; i < zlink::proxy_burst_size; i++) {
        // Forward all the parts of one message
        bool first_proxy_frame = true;
        size_t application_part_index = 0;
        while (true) {
            zlink::pipe_t *source_pipe = NULL;
            int rc = from_->recv_pipe (
              msg_, &source_pipe, ZLINK_DONTWAIT, true);
            proxy_source_pipe_pin_t source_pin (source_pipe);
            if (rc < 0) {
                const int saved_errno = errno;
                rollback_proxy_record (to_, capture_, msg_);
                errno = saved_errno;
                if (likely (saved_errno == EAGAIN && i > 0))
                    return 0; // End of burst

                return -1;
            }

            const bool more = (msg_->flags () & zlink::msg_t::more) != 0;
            const bool router_identity_preamble =
              first_proxy_frame
              && from_->socket_type () == ZLINK_CORE_SOCKET_ROUTER;
            unsigned char request_reply_kind = 0;
            uint64_t request_reply_sequence = 0;
            const bool has_request_reply_metadata =
              msg_->get_request_reply_metadata (
                &request_reply_kind, &request_reply_sequence);
            if (!router_identity_preamble && application_part_index > 0
                && has_request_reply_metadata) {
                reject_proxy_record (from_, to_, capture_, msg_,
                                     source_pin.get (), more);
                errno = EPROTO;
                return -1;
            }

            // Proxy and capture are public raw-message boundaries. Preserve
            // application bytes and multipart flags, but never forward the
            // internal request/reply kind or sequence to either output.
            msg_->reset_request_reply_metadata ();

            if (!router_identity_preamble)
                ++application_part_index;
            first_proxy_frame = false;

            //  Copy message to capture socket if any
            rc = capture (capture_, msg_, more);
            if (unlikely (rc < 0)) {
                const int saved_errno = errno;
                rollback_proxy_record (to_, capture_, msg_);
                errno = saved_errno;
                return -1;
            }

#ifdef ZLINK_BUILD_TESTS
            if (g_fail_next_proxy_destination_send.exchange (
                  false, std::memory_order_acq_rel)) {
                errno = EAGAIN;
                rc = -1;
            } else
#endif
                rc = to_->send (msg_, more ? ZLINK_SNDMORE : 0);
            if (unlikely (rc < 0)) {
                const int saved_errno = errno;
                rollback_proxy_record (to_, capture_, msg_);
                errno = saved_errno;
                return -1;
            }
            if (more == 0)
                break;
        }
    }

    return 0;
}

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
    const zlink::socket_poller_t::output_readiness_t proxy_output_readiness =
      zlink::socket_poller_t::transport_output_readiness;
    //  Poll for everything.
    zlink::socket_poller_t *poller_all =
      new (std::nothrow) zlink::socket_poller_t (
        proxy_output_readiness);
    zlink::socket_poller_t *poller_in = new (std::nothrow) zlink::
      socket_poller_t; //  Poll only 'ZLINK_POLLIN' on all sockets. Initial blocking poll in loop.
    //  All except 'ZLINK_POLLIN' on 'frontend_'.
    zlink::socket_poller_t *poller_receive_blocked =
      new (std::nothrow) zlink::socket_poller_t (
        proxy_output_readiness);

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
        //  All except 'ZLINK_POLLIN' on 'backend_'.
        poller_send_blocked =
          new (std::nothrow) zlink::socket_poller_t (
            proxy_output_readiness);
        //  All except 'ZLINK_POLLIN' on both 'frontend_' and 'backend_'.
        poller_both_blocked = new (std::nothrow)
          zlink::socket_poller_t (
            proxy_output_readiness);
        //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'frontend_'.
        poller_frontend_only = new (std::nothrow)
          zlink::socket_poller_t (
            proxy_output_readiness);
        //  Only 'ZLINK_POLLIN' and 'ZLINK_POLLOUT' on 'backend_'.
        poller_backend_only = new (std::nothrow)
          zlink::socket_poller_t (
            proxy_output_readiness);
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
