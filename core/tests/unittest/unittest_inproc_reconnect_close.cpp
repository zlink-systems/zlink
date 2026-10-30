/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/control_runtime.hpp"
#include "core/ctx.hpp"
#include "utils/clock.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace zlink {
class session_termination_test_access_t {
  public:
    static int retain_async_commands (socket_base_t *socket_)
    {
        return socket_->ensure_async_command_processing (true);
    }
    // The raw socket turn is what the async command owner and the private
    // reconnect callback serialize on. Holding it from the test thread parks
    // either of them at a known point without adding a public API call.
    static void lock_turn (socket_base_t *socket_)
    {
        socket_->lifecycle_coordinator ().lock_public_api_sync ();
    }
    static void unlock_turn (socket_base_t *socket_)
    {
        socket_->lifecycle_coordinator ().unlock_public_api_sync ();
    }
    static bool reconnect_due_at (socket_base_t *socket_, uint64_t *deadline_)
    {
        socket_inproc_reconnect_runtime_t &runtime = socket_->inproc_reconnect_runtime ();
        scoped_lock_t lock (runtime.sync);
        if (runtime.pending.empty ())
            return false;
        *deadline_ = runtime.pending.begin ()->first;
        return true;
    }
    static bool reconnect_stopping (socket_base_t *socket_)
    {
        socket_inproc_reconnect_runtime_t &runtime = socket_->inproc_reconnect_runtime ();
        scoped_lock_t lock (runtime.sync);
        return runtime.stopping;
    }
};
}

namespace {
typedef zlink::session_termination_test_access_t access_t;

const int reconnect_ivl_ms = 250;

struct control_probe_t {
    std::atomic<uint64_t> runs;
    control_probe_t () : runs (0) {}
};

void control_probe_main (void *arg_)
{
    static_cast<control_probe_t *> (arg_)->runs.fetch_add (1);
}

struct close_call_t {
    void *socket;
    std::atomic<bool> done;
    std::atomic<int> result;
    close_call_t (void *socket_) : socket (socket_), done (false), result (-1) {}
    void run ()
    {
        result.store (static_cast<int> (zlink_close (socket)));
        done.store (true);
    }
};

template<class Predicate> bool wait_until (Predicate predicate_)
{
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!predicate_ () && std::chrono::steady_clock::now () < end)
        std::this_thread::yield ();
    return predicate_ ();
}

void set_int_option (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, option_, &value_, sizeof value_));
}

//  A ROUTER connected to one inproc peer. ivl == ivl_max disables jitter, and
//  a retained async command owner processes the peer detach without an
//  application call on the source.
void *connected_source (const char *endpoint_, void **peer_out_)
{
    void *source = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_int_option (source, ZLINK_OPT_LINGER, 0);
    set_int_option (source, ZLINK_OPT_RECONNECT_IVL, reconnect_ivl_ms);
    set_int_option (source, ZLINK_OPT_RECONNECT_IVL_MAX, reconnect_ivl_ms);
    void *peer = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_int_option (peer, ZLINK_OPT_LINGER, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (peer, endpoint_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (source, endpoint_));
    TEST_ASSERT_EQUAL_INT (
      0, access_t::retain_async_commands (as_socket_handle (source).socket));
    *peer_out_ = peer;
    return source;
}
}

void setUp () { setup_test_context (); }
void tearDown () { teardown_test_context (); }

//  Close is the only public call on the source. The private reconnect
//  callback is already running on the control thread, parked on the socket
//  turn, when close starts. That callback is not a public API on the handle,
//  so close must be admitted rather than return BUSY.
void test_close_is_admitted_while_reconnect_callback_runs ()
{
    void *peer = NULL;
    void *source = connected_source ("inproc://reconnect-close-callback", &peer);
    zlink::socket_base_t *socket = as_socket_handle (source).socket;
    zlink::control_runtime_t *control = socket->get_ctx ()->control_runtime ();
    TEST_ASSERT_NOT_NULL (control);

    test_context_socket_close (peer);
    uint64_t deadline = 0;
    TEST_ASSERT_TRUE_MESSAGE (
      wait_until ([&] () { return access_t::reconnect_due_at (socket, &deadline); }),
      "peer detach did not schedule an inproc reconnect");

    access_t::lock_turn (socket);
    zlink::clock_t clock;
    const bool scheduled_ahead = clock.now_ms () < deadline;
    uint64_t still_due = 0;
    const bool still_pending = access_t::reconnect_due_at (socket, &still_due);

    //  The control thread runs one task at a time. A 1 ms probe that stops
    //  advancing after the reconnect deadline shows that the control thread
    //  is inside the reconnect callback, parked on the held turn.
    control_probe_t probe;
    const uint64_t probe_id =
      control->add_periodic_task (&control_probe_main, &probe, 1, true);
    const bool probe_passed_deadline =
      probe_id != 0 && wait_until ([&] () { return probe.runs.load () != 0 && clock.now_ms () > deadline; });
    bool callback_parked = false;
    for (int i = 0; probe_passed_deadline && !callback_parked && i < 60; ++i) {
        const uint64_t runs = probe.runs.load ();
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        callback_parked = probe.runs.load () == runs;
    }

    close_call_t close_call (source);
    std::thread closer (&close_call_t::run, &close_call);
    const bool close_left_busy_window = wait_until ([&] () {
        return close_call.done.load () || access_t::reconnect_stopping (socket);
    });
    const bool busy_before_release =
      close_call.done.load () && close_call.result.load () == ZLINK_CLOSE_BUSY;

    access_t::unlock_turn (socket);
    closer.join ();
    (void) control->remove_task (probe_id);
    if (close_call.result.load () == ZLINK_CLOSE_OK)
        test_context_socket_mark_closed (source);

    TEST_ASSERT_TRUE_MESSAGE (scheduled_ahead && still_pending,
                              "reconnect deadline passed before the turn was held");
    TEST_ASSERT_TRUE_MESSAGE (probe_passed_deadline && callback_parked,
                              "reconnect callback did not start on the control thread");
    TEST_ASSERT_TRUE (close_left_busy_window);
    TEST_ASSERT_FALSE_MESSAGE (busy_before_release,
                               "private reconnect callback made close BUSY");
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_call.result.load ());
}

//  Close stops the reconnect scheduler as soon as it is accepted, before the
//  async command owner quiesces. A peer detach that this owner processes in
//  that interval must not schedule a reconnect and must not be treated as a
//  lost live intent.
void test_peer_detach_after_close_accept_is_not_scheduled ()
{
    void *peer = NULL;
    void *source = connected_source ("inproc://reconnect-close-detach", &peer);
    zlink::socket_base_t *socket = as_socket_handle (source).socket;

    access_t::lock_turn (socket);
    close_call_t peer_close (peer);
    std::thread peer_closer (&close_call_t::run, &peer_close);
    const bool peer_closed = wait_until ([&] () { return peer_close.done.load (); });

    close_call_t source_close (source);
    std::thread source_closer (&close_call_t::run, &source_close);
    const bool stopped_while_turn_held =
      wait_until ([&] () { return access_t::reconnect_stopping (socket); });
    const bool source_returned_early = source_close.done.load ();

    access_t::unlock_turn (socket);
    peer_closer.join ();
    source_closer.join ();
    if (peer_close.result.load () == ZLINK_CLOSE_OK)
        test_context_socket_mark_closed (peer);
    if (source_close.result.load () == ZLINK_CLOSE_OK)
        test_context_socket_mark_closed (source);

    TEST_ASSERT_TRUE_MESSAGE (peer_closed, "peer close waited for the source turn");
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, peer_close.result.load ());
    TEST_ASSERT_TRUE_MESSAGE (stopped_while_turn_held,
                              "accepted close did not stop the reconnect scheduler");
    TEST_ASSERT_FALSE (source_returned_early);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, source_close.result.load ());
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_close_is_admitted_while_reconnect_callback_runs);
    RUN_TEST (test_peer_detach_after_close_accept_is_not_scheduled);
    return UNITY_END ();
}
