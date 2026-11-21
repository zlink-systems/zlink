/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <zlink.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

// Core allows exactly one zlink_ctx_term per context, and one destroy per
// timer or monitor. Concurrent close calls must reach Core once. The wrappers
// forward only the first call, so a regression fails the count instead of
// corrupting a freed handle.
extern "C" zlink_close_result_t __real_zlink_ctx_term (void *);
extern "C" zlink_close_result_t __real_zlink_timer_destroy (void **);
extern "C" zlink_close_result_t __real_zlink_monitor_close (void **);

namespace
{
std::atomic_int term_calls{0};
std::atomic_int timer_destroy_calls{0};
std::atomic_int monitor_close_calls{0};
std::atomic_bool forward_all_timer_destroy_calls{false};

void hold ()
{
    std::this_thread::sleep_for (std::chrono::milliseconds (2));
}
}

extern "C" zlink_close_result_t __wrap_zlink_ctx_term (void *context_)
{
    const int call = ++term_calls;
    hold ();
    return call == 1 ? __real_zlink_ctx_term (context_) : ZLINK_CLOSE_OK;
}

extern "C" zlink_close_result_t __wrap_zlink_timer_destroy (void **timer_)
{
    const int call = ++timer_destroy_calls;
    hold ();
    return call == 1 || forward_all_timer_destroy_calls.load ()
             ? __real_zlink_timer_destroy (timer_)
             : ZLINK_CLOSE_OK;
}

extern "C" zlink_close_result_t __wrap_zlink_monitor_close (void **monitor_)
{
    const int call = ++monitor_close_calls;
    hold ();
    return call == 1 ? __real_zlink_monitor_close (monitor_) : ZLINK_CLOSE_OK;
}

namespace
{
constexpr int thread_count = 16;

template <class Fn> void run_concurrently (Fn fn_)
{
    std::atomic_bool go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i)
        threads.emplace_back ([&] {
            while (!go.load ())
                std::this_thread::yield ();
            try {
                fn_ ();
            }
            catch (...) {
                assert (false);
            }
        });
    go = true;
    for (auto &thread : threads)
        thread.join ();
}

void test_context_term_once ()
{
    for (int i = 0; i < 50; ++i) {
        term_calls = 0;
        zlink::context_t ctx;
        run_concurrently ([&] { ctx.term (); });
        assert (term_calls == 1);
        assert (!ctx.valid ());
        ctx.term ();
        assert (term_calls == 1);
    }
}

void test_timer_close_once ()
{
    for (int i = 0; i < 50; ++i) {
        timer_destroy_calls = 0;
        zlink::timer_t timer;
        run_concurrently ([&] { timer.close (); });
        assert (timer_destroy_calls == 1);
        assert (!timer.valid ());
    }
}

void test_timer_close_busy_is_retryable ()
{
    timer_destroy_calls = 0;
    forward_all_timer_destroy_calls = true;
    zlink::timer_t timer;
    zlink::poller_t poller;
    poller.add (timer, 1);

    bool busy = false;
    try {
        timer.close ();
    }
    catch (const zlink::close_error_t &error) {
        busy = error.result () == zlink::close_result_t::busy;
    }
    assert (busy);
    assert (timer.valid ());
    assert (poller.remove (timer));
    timer.close ();
    assert (timer_destroy_calls == 2);
    assert (!timer.valid ());
    forward_all_timer_destroy_calls = false;
}

void test_monitor_close_once ()
{
    for (int i = 0; i < 50; ++i) {
        monitor_close_calls = 0;
        zlink::context_t ctx;
        {
            zlink::pair_socket_t socket (ctx);
            zlink::socket_monitor_t monitor = socket.monitor_open (zlink::monitor_event::all);
            assert (monitor.valid ());
            run_concurrently ([&] { monitor.close (); });
            assert (monitor_close_calls == 1);
            assert (!monitor.valid ());
        }
        ctx.term ();
    }
}
}

int main ()
{
    test_timer_close_busy_is_retryable ();
    test_context_term_once ();
    test_timer_close_once ();
    test_monitor_close_once ();
    return 0;
}
