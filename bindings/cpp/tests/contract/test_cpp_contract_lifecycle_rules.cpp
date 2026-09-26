/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <zlink.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <unistd.h>

extern "C" zlink_close_result_t __real_zlink_poller_destroy (void **);
extern "C" int
__real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);
extern "C" zlink_close_result_t __real_zlink_ctx_shutdown (void *);
extern "C" zlink_close_result_t __real_zlink_ctx_term (void *);

namespace
{
std::atomic_bool reject_destroy{false};
std::atomic_bool interrupt_wait{false};
std::atomic_int shutdown_order{0};
std::atomic_int term_order{0};
std::atomic_int call_order{0};
}

extern "C" zlink_close_result_t __wrap_zlink_poller_destroy (void **handle_)
{
    if (reject_destroy.exchange (false)) {
        errno = EBUSY;
        return ZLINK_CLOSE_BUSY;
    }
    return __real_zlink_poller_destroy (handle_);
}

extern "C" int __wrap_zlink_poller_wait (void *poller_,
                                         zlink_poller_event_t *events_,
                                         int capacity_,
                                         long timeout_,
                                         zlink_config_result_t *error_)
{
    if (interrupt_wait.exchange (false)) {
        if (error_)
            *error_ = ZLINK_CONFIG_INTERNAL_ERROR;
        errno = EINTR;
        return -1;
    }
    return __real_zlink_poller_wait (poller_, events_, capacity_, timeout_, error_);
}

extern "C" zlink_close_result_t __wrap_zlink_ctx_shutdown (void *context_)
{
    shutdown_order = ++call_order;
    return __real_zlink_ctx_shutdown (context_);
}

extern "C" zlink_close_result_t __wrap_zlink_ctx_term (void *context_)
{
    term_order = ++call_order;
    return __real_zlink_ctx_term (context_);
}

int main ()
{
    zlink::poller_t poller;
    assert (poller.valid ());
    reject_destroy = true;
    bool busy = false;
    try {
        poller.close ();
    }
    catch (const zlink::close_error_t &error_) {
        busy = error_.result () == zlink::close_result_t::busy && error_.internal_errno () == EBUSY;
    }
    assert (busy && poller.valid ());

    int fds[2];
    assert (pipe (fds) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    interrupt_wait = true;
    bool interrupted = false;
    try {
        zlink::poll_event_t event;
        (void) poller.wait (&event, 1, std::chrono::milliseconds (0));
    }
    catch (const zlink::config_error_t &error_) {
        interrupted = error_.result () == zlink::config_result_t::internal_error
                      && error_.internal_errno () == EINTR;
    }
    assert (interrupted);
    poller.remove_fd (fds[0]);
    close (fds[0]);
    close (fds[1]);

    poller.close ();
    assert (!poller.valid ());

    zlink::context_t context;
    context.term ();
    assert (shutdown_order > 0 && term_order > shutdown_order);
    assert (!context.valid ());

    zlink::context_t already_shutdown;
    already_shutdown.shutdown ();
    already_shutdown.term ();
    assert (!already_shutdown.valid ());
}
