/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <zlink.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C" int
__real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);

namespace
{
#define CHECK(condition_)                                                                          \
    do {                                                                                           \
        if (!(condition_)) {                                                                       \
            std::fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition_);                 \
            std::exit (1);                                                                         \
        }                                                                                          \
    } while (false)

std::atomic_bool inject_wait_error{false};

template <typename Action> bool has_config_result (Action &&action_, zlink::config_result_t result_)
{
    try {
        action_ ();
    }
    catch (const zlink::config_error_t &error_) {
        return error_.result () == result_;
    }
    return false;
}

void test_duplicate_source_uses_core_conflict ()
{
    zlink::poller_t poller;
    int fds[2];
    CHECK (pipe (fds) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    CHECK (has_config_result ([&] { poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0); },
                              zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove_fd (fds[0]));
    CHECK (
      has_config_result ([&] { poller.remove_fd (fds[0]); }, zlink::config_result_t::not_found));
    CHECK (has_config_result ([&] { poller.modify_fd (fds[0], zlink::poll_event_flag_t::pollin); },
                              zlink::config_result_t::not_found));
    close (fds[0]);
    close (fds[1]);

    zlink::context_t context;
    zlink::pair_socket_t socket (context);
    poller.add (socket, zlink::poll_event_flag_t::pollin, 1);
    CHECK (has_config_result ([&] { poller.add (socket, zlink::poll_event_flag_t::pollin, 2); },
                              zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove (socket));

    zlink::timer_t timer;
    poller.add (timer, 3);
    CHECK (has_config_result ([&] { poller.add (timer, 4); }, zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove (timer));
}

void test_active_wait_uses_core_busy ()
{
    zlink::poller_t poller;
    int fds[2];
    int other[2];
    CHECK (pipe (fds) == 0 && pipe (other) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    std::thread waiter ([&] {
        zlink::poll_event_t event;
        CHECK (poller.wait (&event, 1, std::chrono::milliseconds (-1)) == 1);
    });

    // The public size result is the signal that Core has entered the wait.
    bool core_busy = false;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!core_busy && std::chrono::steady_clock::now () < deadline) {
        core_busy =
          has_config_result ([&] { (void) poller.size (); }, zlink::config_result_t::busy);
        if (!core_busy)
            std::this_thread::yield ();
    }
    bool add_busy = false;
    bool modify_busy = false;
    bool remove_busy = false;
    bool wait_busy = false;
    if (core_busy) {
        add_busy =
          has_config_result ([&] { poller.add_fd (other[0], zlink::poll_event_flag_t::pollin, 0); },
                             zlink::config_result_t::busy);
        modify_busy =
          has_config_result ([&] { poller.modify_fd (fds[0], zlink::poll_event_flag_t::pollin); },
                             zlink::config_result_t::busy);
        remove_busy =
          has_config_result ([&] { poller.remove_fd (fds[0]); }, zlink::config_result_t::busy);
        wait_busy = has_config_result (
          [&] {
              zlink::poll_event_t event;
              (void) poller.wait (&event, 1, std::chrono::milliseconds (0));
          },
          zlink::config_result_t::busy);
    }
    const char wake = 1;
    CHECK (write (fds[1], &wake, 1) == 1);
    waiter.join ();
    CHECK (core_busy && add_busy && modify_busy && remove_busy && wait_busy);
    CHECK (poller.size () == 1);
    CHECK (poller.remove_fd (fds[0]));
    close (fds[0]);
    close (fds[1]);
    close (other[0]);
    close (other[1]);
}

void test_wait_uses_error_out ()
{
    zlink::poller_t poller;
    int fds[2];
    CHECK (pipe (fds) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    inject_wait_error = true;
    bool preserved = false;
    try {
        zlink::poll_event_t event;
        (void) poller.wait (&event, 1, std::chrono::milliseconds (0));
    }
    catch (const zlink::config_error_t &error_) {
        preserved = error_.result () == zlink::config_result_t::not_found
                    && error_.internal_errno () == EBUSY;
    }
    CHECK (preserved);
    CHECK (poller.remove_fd (fds[0]));
    close (fds[0]);
    close (fds[1]);
}
} // namespace

extern "C" int __wrap_zlink_poller_wait (void *poller_,
                                         zlink_poller_event_t *events_,
                                         int capacity_,
                                         long timeout_,
                                         zlink_config_result_t *error_)
{
    if (inject_wait_error.exchange (false)) {
        if (error_)
            *error_ = ZLINK_CONFIG_NOT_FOUND;
        errno = EBUSY;
        return -1;
    }
    return __real_zlink_poller_wait (poller_, events_, capacity_, timeout_, error_);
}

int main (int argc_, char **argv_)
{
    if (argc_ == 1 || std::strcmp (argv_[1], "duplicate") == 0)
        test_duplicate_source_uses_core_conflict ();
    if (argc_ == 1 || std::strcmp (argv_[1], "concurrency") == 0)
        test_active_wait_uses_core_busy ();
    if (argc_ == 1 || std::strcmp (argv_[1], "projection") == 0)
        test_wait_uses_error_out ();
}
