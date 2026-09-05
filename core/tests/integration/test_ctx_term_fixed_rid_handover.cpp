/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const char fixed_rid[] =
  "actor-join-source-0123456789abcdef0123456789abcd";
const int default_rounds = 20;
const int initial_admission_budget_ms = 2000;
const int default_handover_budget_ms = 2000;
const int default_p95_limit_ms = 200;

struct sample_t
{
    int elapsed_ms;
    bool admitted;
};

int env_int (const char *name_, int default_)
{
    const char *const value = getenv (name_);
    return value && *value ? atoi (value) : default_;
}

bool selected_value (const char *name_, const char *value_)
{
    const char *const selected = getenv (name_);
    return !selected || !*selected || strcmp (selected, value_) == 0;
}

void set_int_option (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, option_, &value_, sizeof value_));
}

void close_zero_linger (void *socket_)
{
    set_int_option (socket_, ZLINK_OPT_LINGER, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket_));
}

void *make_router (void *ctx_, const char *bind_address_, char *endpoint_)
{
    void *const router = zlink_socket (ctx_, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    set_int_option (router, ZLINK_OPT_LINGER, 0);
    set_int_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, handover);
    set_int_option (router, ZLINK_OPT_RCVTIMEO, 1);
    test_bind (router, bind_address_, endpoint_, MAX_SOCKET_STRING);
    return router;
}

void *make_dealer (void *ctx_, const char *endpoint_, int reconnect_ivl_,
                   bool configure_reconnect_)
{
    void *const dealer = zlink_socket (ctx_, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    set_int_option (dealer, ZLINK_OPT_LINGER, 0);
    if (configure_reconnect_) {
        set_int_option (dealer, ZLINK_OPT_RECONNECT_IVL, reconnect_ivl_);
        set_int_option (dealer, ZLINK_OPT_RECONNECT_IVL_MAX, reconnect_ivl_);
    }
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, fixed_rid, strlen (fixed_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_));
    return dealer;
}

void send_marker (void *dealer_, char marker_)
{
    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, 1));
    *static_cast<char *> (zlink_msg_data (&msg)) = marker_;
    const zlink_submit_result_t rc = zlink_send_part (
      dealer_, &msg, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL, NULL);
    if (rc != ZLINK_SUBMIT_OK)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
}

bool recv_marker (void *router_, char marker_)
{
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    const zlink_recv_result_t rc = zlink_router_recv_part (
      router_, &source, &token, &msg, &part_flag, ZLINK_RECV_FLAGS_NONE);
    bool matched = false;
    if (rc == ZLINK_RECV_OK) {
        TEST_ASSERT_NOT_NULL (source);
        TEST_ASSERT_EQUAL_UINT8 (strlen (fixed_rid), source->size);
        TEST_ASSERT_EQUAL_MEMORY (fixed_rid, source->data, source->size);
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
        matched = zlink_msg_size (&msg) == 1
                  && *static_cast<const char *> (zlink_msg_data (&msg))
                       == marker_;
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    return matched;
}

sample_t wait_for_marker (void *dealer_, void *router_, char marker_,
                          int budget_ms_)
{
    const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now ();
    int sends = 0;
    for (;;) {
        const int elapsed = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - start)
            .count ());
        if (elapsed >= budget_ms_)
            return sample_t{elapsed, false};

        // Keep the probe rate bounded while the ROUTER receive timeout is the
        // synchronization point. A unique payload distinguishes replacement
        // traffic from anything queued on the prior pipe.
        if ((sends++ % 5) == 0)
            send_marker (dealer_, marker_);
        if (recv_marker (router_, marker_)) {
            const int admitted_ms = static_cast<int> (
              std::chrono::duration_cast<std::chrono::milliseconds> (
                std::chrono::steady_clock::now () - start)
                .count ());
            return sample_t{admitted_ms, true};
        }
    }
}

sample_t run_one (const char *bind_address_, int reconnect_ivl_, int round_)
{
    void *const ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    char endpoint[MAX_SOCKET_STRING];
    void *const router = make_router (ctx, bind_address_, endpoint);
    void *const prior = make_dealer (ctx, endpoint, reconnect_ivl_, true);

    const sample_t initial = wait_for_marker (
      prior, router, 'A', initial_admission_budget_ms);
    TEST_ASSERT_TRUE_MESSAGE (initial.admitted,
                              "initial DEALER was not admitted");

    // The matrix varies only the live prior's reconnect policy. Keep the
    // replacement at the public default so the independent variable remains
    // unambiguous.
    void *const replacement = make_dealer (ctx, endpoint, 0, false);
    const sample_t result = wait_for_marker (
      replacement, router, 'B',
      env_int ("ZLINK_HANDOVER_BUDGET_MS", default_handover_budget_ms));

    close_zero_linger (replacement);
    close_zero_linger (prior);
    close_zero_linger (router);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, zlink_ctx_term (ctx),
                                   "context termination failed");
    if (!result.admitted)
        fprintf (stderr,
                 "handover timeout transport=%s ivl=%d round=%d elapsed=%dms\n",
                 bind_address_, reconnect_ivl_, round_, result.elapsed_ms);
    return result;
}

int percentile95 (std::vector<int> values_)
{
    std::sort (values_.begin (), values_.end ());
    const size_t index = (values_.size () * 95 + 99) / 100 - 1;
    return values_[index];
}

void run_cell (const char *transport_, const char *bind_address_,
               int reconnect_ivl_)
{
    const int rounds = env_int ("ZLINK_HANDOVER_ROUNDS", default_rounds);
    TEST_ASSERT_GREATER_THAN_INT (0, rounds);
    std::vector<int> values;
    values.reserve (static_cast<size_t> (rounds));
    int admitted = 0;
    for (int round = 0; round < rounds; ++round) {
        const sample_t sample = run_one (bind_address_, reconnect_ivl_, round);
        values.push_back (sample.elapsed_ms);
        admitted += sample.admitted ? 1 : 0;
    }

    std::vector<int> sorted = values;
    std::sort (sorted.begin (), sorted.end ());
    const int p50 = sorted[(sorted.size () - 1) / 2];
    const int p95 = percentile95 (values);
    fprintf (stdout,
             "HANDOVER_DISTRIBUTION transport=%s reconnect_ivl_ms=%d "
             "rounds=%d admitted=%d min=%d p50=%d p95=%d max=%d ms\n",
             transport_, reconnect_ivl_, rounds, admitted, sorted.front (),
             p50, p95, sorted.back ());
    fflush (stdout);

    TEST_ASSERT_EQUAL_INT_MESSAGE (rounds, admitted,
                                   "replacement admission timed out");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE (
      env_int ("ZLINK_HANDOVER_ASSERT_P95_MS", default_p95_limit_ms) + 1,
      p95, "replacement admission p95 exceeded the bound");
}
}

void test_fixed_rid_handover_admission_distribution ()
{
    const int reconnect_intervals[] = {10, 100, 1000};
    for (size_t i = 0; i < sizeof reconnect_intervals / sizeof reconnect_intervals[0];
         ++i) {
        char ivl_text[16];
        snprintf (ivl_text, sizeof ivl_text, "%d", reconnect_intervals[i]);
        if (!selected_value ("ZLINK_HANDOVER_IVL", ivl_text))
            continue;
        if (selected_value ("ZLINK_HANDOVER_TRANSPORT", "tcp"))
            run_cell ("tcp", "tcp://127.0.0.1:*", reconnect_intervals[i]);
        if (selected_value ("ZLINK_HANDOVER_TRANSPORT", "inproc"))
            run_cell ("inproc", "inproc://fixed-rid-handover",
                      reconnect_intervals[i]);
    }
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
    RUN_TEST (test_fixed_rid_handover_admission_distribution);
    return UNITY_END ();
}
