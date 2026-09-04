/* SPDX-License-Identifier: MPL-2.0 */

// Context termination after fixed-RID DEALER handover cycles on a HANDOVER
// ROUTER (public C API only).
//
// Background: .NET framework unit tests hung in zlink_ctx_term() after a
// ROUTER + fixed-RID DEALER handover scenario. The hang dump showed one
// DEALER that managed code never closed. These cases pin the Core contract:
//  - with every socket closed (linger 0), zlink_ctx_term() returns promptly
//    after repeated same-RID occupation / exact-disconnect cycles, with the
//    retired DEALER's reconnect intent alive or disabled, on tcp and inproc;
//  - with one socket left open, zlink_ctx_term() blocks until that socket is
//    closed from another thread, then returns (spec: sockets must be closed
//    before the context terminates).

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace
{
const int ctx_term_watchdog_ms = 10000;
const int admission_probe_ms = 100;
const int handover_cycles = 3;
//  Same-RID replacement admission over tcp while the prior pipe is alive is
//  observed to take 100 ms .. >5 s (inproc: immediate). That latency is not
//  the subject here, so the pre-disconnect probe is bounded and best-effort;
//  admission after the prior's exact disconnect is asserted strictly.
const int pre_disconnect_admission_budget_ms = 600;
const int post_disconnect_admission_budget_ms = 5000;
const char fixed_rid[] = "actor-join-source-0123456789abcdef0123456789abcd"; // 50 bytes like the framework

bool should_run_case (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

struct term_watchdog_t
{
    void *ctx;
    std::atomic<bool> done;
    std::atomic<int> rc;
    std::thread thread;

    explicit term_watchdog_t (void *ctx_) : ctx (ctx_), done (false), rc (-1)
    {
        thread = std::thread ([this] () {
            rc.store (zlink_ctx_term (ctx));
            done.store (true);
        });
    }

    //  Returns true when zlink_ctx_term() returned within timeout_ms_.
    bool wait (int timeout_ms_)
    {
        for (int waited = 0; waited < timeout_ms_ && !done.load (); waited += 10)
            msleep (10);
        return done.load ();
    }

    ~term_watchdog_t ()
    {
        if (done.load ())
            thread.join ();
        else
            thread.detach (); // the test already failed; do not block exit
    }
};

void close_socket_zero_linger (void *socket_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket_));
}

void *make_router (void *ctx_)
{
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int mandatory = 1;
    const int zero = 0;
    const int probe_timeout = admission_probe_ms;
    const int64_t unlimited = -1;
    void *router = zlink_socket (ctx_, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router, "actor-join-target", 17));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof handover));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof mandatory));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_MAXMSGSIZE, &unlimited, sizeof unlimited));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVTIMEO, &probe_timeout, sizeof probe_timeout));
    return router;
}

void *make_fixed_rid_dealer (void *ctx_, const char *endpoint_)
{
    void *dealer = zlink_socket (ctx_, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, fixed_rid, strlen (fixed_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_));
    return dealer;
}

void set_reconnect_ivl (void *dealer_, int ivl_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_, ZLINK_OPT_RECONNECT_IVL, &ivl_, sizeof ivl_));
}

//  One "hello" part from dealer_ (DONTWAIT); a not-yet-connected DEALER is
//  reported through the submit result, never through errno.
void send_hello_dontwait (void *dealer_)
{
    zlink_msg_t hello;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&hello, 5));
    memcpy (zlink_msg_data (&hello), "hello", 5);
    const zlink_submit_result_t rc = zlink_send_part (
      dealer_, &hello, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL, NULL);
    if (rc != ZLINK_SUBMIT_OK)
        zlink_msg_close (&hello);
}

//  Receives one routed part on the router (bounded by RCVTIMEO). Returns true
//  when a part from the fixed RID arrived.
bool router_recv_hello (void *router_, int flags_)
{
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_recv_result_t rc = zlink_router_recv_part (
      router_, &source, &token, &part, &has_more, static_cast<zlink_recv_flags_t> (flags_));
    bool matched = false;
    if (rc == ZLINK_RECV_OK) {
        TEST_ASSERT_NOT_NULL (source);
        TEST_ASSERT_EQUAL_UINT8 (static_cast<uint8_t> (strlen (fixed_rid)), source->size);
        TEST_ASSERT_EQUAL_MEMORY (fixed_rid, source->data, source->size);
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
        TEST_ASSERT_EQUAL_STRING_LEN ("hello", static_cast<const char *> (zlink_msg_data (&part)), 5);
        matched = true;
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    return matched;
}

//  Sends "hello" from dealer_ until the router receives one from the fixed
//  RID (bounded by budget_ms_); returns true when the route is admitted.
bool send_hello_until_admitted (void *dealer_, void *router_, int budget_ms_)
{
    for (int i = 0; i < budget_ms_ / admission_probe_ms; ++i) {
        send_hello_dontwait (dealer_);
        if (router_recv_hello (router_, ZLINK_RECV_FLAGS_NONE))
            return true;
    }
    return false;
}

void drain_router (void *router_)
{
    while (router_recv_hello (router_, ZLINK_RECV_FLAGS_DONTWAIT)) {
    }
}

//  Mirrors ConnectedRuntime.HandoverAsync(quiescePrior) followed by
//  SendPriorHelloAndDisconnectAsync + SendIdempotentHelloAsync, cycles_ times.
//  keep_prior_reconnect_alive_ leaves the retired DEALER's reconnect intent
//  armed during the replacement's admission instead of disabling it.
void *run_handover_cycles (void *router_,
                           void *first_dealer_,
                           const char *endpoint_,
                           void *ctx_,
                           int cycles_,
                           bool keep_prior_reconnect_alive_)
{
    void *current = first_dealer_;
    for (int cycle = 0; cycle < cycles_; ++cycle) {
        void *prior = current;
        void *replacement = make_fixed_rid_dealer (ctx_, endpoint_);
        if (!keep_prior_reconnect_alive_)
            set_reconnect_ivl (prior, -1);
        (void) send_hello_until_admitted (replacement, router_,
                                          pre_disconnect_admission_budget_ms);
        current = replacement;

        //  Old hello from the retired pipe, then exact disconnect (linger 0).
        set_reconnect_ivl (prior, 0);
        send_hello_dontwait (prior);
        close_socket_zero_linger (prior);

        msleep (50);
        drain_router (router_);
        TEST_ASSERT_TRUE_MESSAGE (
          send_hello_until_admitted (current, router_, post_disconnect_admission_budget_ms),
          "current DEALER lost its route after prior disconnect");
    }
    return current;
}

void run_all_closed_case (const char *bind_address_, bool keep_prior_reconnect_alive_)
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *router = make_router (ctx);
    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, bind_address_, endpoint, sizeof endpoint);

    void *dealer = make_fixed_rid_dealer (ctx, endpoint);
    TEST_ASSERT_TRUE (
      send_hello_until_admitted (dealer, router, post_disconnect_admission_budget_ms));
    dealer = run_handover_cycles (router, dealer, endpoint, ctx, handover_cycles,
                                  keep_prior_reconnect_alive_);

    //  Framework order: source DEALER, then the ROUTER owner, then the context.
    close_socket_zero_linger (dealer);
    close_socket_zero_linger (router);

    term_watchdog_t term (ctx);
    TEST_ASSERT_TRUE_MESSAGE (term.wait (ctx_term_watchdog_ms),
                              "zlink_ctx_term hung with every socket closed");
    TEST_ASSERT_EQUAL_INT (0, term.rc.load ());
}
}

void test_ctx_term_returns_after_fixed_rid_handover_cycles_tcp ()
{
    run_all_closed_case ("tcp://127.0.0.1:*", false);
}

void test_ctx_term_returns_after_fixed_rid_handover_cycles_tcp_reconnect_alive ()
{
    run_all_closed_case ("tcp://127.0.0.1:*", true);
}

void test_ctx_term_returns_after_fixed_rid_handover_cycles_inproc ()
{
    run_all_closed_case ("inproc://ctx-term-fixed-rid-handover", true);
}

//  Documents the behaviour behind the framework hang: a DEALER that is never
//  closed keeps zlink_ctx_term() blocked, and a late zlink_close() from
//  another thread releases it.
void test_ctx_term_blocks_until_unclosed_dealer_is_closed ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *router = make_router (ctx);
    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "tcp://127.0.0.1:*", endpoint, sizeof endpoint);

    void *dealer = make_fixed_rid_dealer (ctx, endpoint);
    TEST_ASSERT_TRUE (
      send_hello_until_admitted (dealer, router, post_disconnect_admission_budget_ms));
    dealer = run_handover_cycles (router, dealer, endpoint, ctx, 2, true);

    //  Leaked replacement: created, connected, never closed (the framework
    //  test helper dropped it after an admission timeout).
    void *leaked = make_fixed_rid_dealer (ctx, endpoint);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (leaked, ZLINK_OPT_LINGER, &zero, sizeof zero));

    close_socket_zero_linger (dealer);
    close_socket_zero_linger (router);

    term_watchdog_t term (ctx);
    TEST_ASSERT_FALSE_MESSAGE (term.wait (500),
                               "zlink_ctx_term returned with a socket still open");

    //  The user is still responsible for closing the socket; the context then
    //  finishes termination.
    //  Option changes are refused with ETERM once termination began, so the
    //  linger was set before; zlink_close() itself is still accepted.
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (leaked));
    TEST_ASSERT_TRUE_MESSAGE (term.wait (ctx_term_watchdog_ms),
                              "zlink_ctx_term did not return after the late close");
    TEST_ASSERT_EQUAL_INT (0, term.rc.load ());
}

void setUp ()
{
}

void tearDown ()
{
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
#define RUN_SELECTED(test_)                                                    \
    do {                                                                       \
        if (should_run_case (#test_))                                          \
            RUN_TEST (test_);                                                  \
    } while (false)

    RUN_SELECTED (test_ctx_term_returns_after_fixed_rid_handover_cycles_tcp);
    RUN_SELECTED (
      test_ctx_term_returns_after_fixed_rid_handover_cycles_tcp_reconnect_alive);
    RUN_SELECTED (test_ctx_term_returns_after_fixed_rid_handover_cycles_inproc);
    RUN_SELECTED (test_ctx_term_blocks_until_unclosed_dealer_is_closed);
#undef RUN_SELECTED
    return UNITY_END ();
}
