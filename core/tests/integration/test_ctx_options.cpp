/* SPDX-License-Identifier: MPL-2.0 */

#include <chrono>
#include <future>
#include <limits>
#include <vector>
#include <string.h>
#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

#define WAIT_FOR_BACKGROUND_THREAD_INSPECTION (0)

static void read_socket_auto_hwm_snapshot (void *socket_, zlink_monitor_status_t *out_);

static uint64_t get_u64_context_option (void *ctx_, zlink_ctx_option_t option_)
{
    uint64_t value = UINT64_MAX;
    size_t value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_get_data (ctx_, option_, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (value), value_size);
    return value;
}

#ifdef ZLINK_HAVE_LINUX
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h> // for sleep()
#include <sched.h>

#define TEST_POLICY (SCHED_OTHER) // NOTE: SCHED_OTHER is the default Linux scheduler

bool is_allowed_to_raise_priority ()
{
    // NOTE1: if setrlimit() fails with EPERM, this means that current user has not enough permissions.
    // NOTE2: even for privileged users (e.g., root) getrlimit() would usually return 0 as nice limit; the only way to
    //        discover if the user is able to increase the nice value is to actually try to change the rlimit:
    struct rlimit rlim;
    rlim.rlim_cur = 40;
    rlim.rlim_max = 40;
    if (setrlimit (RLIMIT_NICE, &rlim) == 0) {
        // rlim_cur == 40 means that this process is allowed to set a nice value of -20
        if (WAIT_FOR_BACKGROUND_THREAD_INSPECTION)
            printf ("This process has enough permissions to raise ZLINK "
                    "background thread priority!\n");
        return true;
    }

    if (WAIT_FOR_BACKGROUND_THREAD_INSPECTION)
        printf ("This process has NOT enough permissions to raise ZLINK "
                "background thread priority.\n");
    return false;
}

#else

#define TEST_POLICY (0)

bool is_allowed_to_raise_priority ()
{
    return false;
}

#endif


void test_ctx_thread_opts ()
{
    // verify that setting negative values (e.g., default values) fail:
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT,
                           zlink_ctx_set (get_test_context (), ZLINK_THREAD_SCHED_POLICY,
                                          ZLINK_THREAD_SCHED_POLICY_DFLT));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set (get_test_context (), ZLINK_THREAD_PRIORITY, ZLINK_THREAD_PRIORITY_DFLT));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);


    // test scheduling policy:

    // set context options that alter the background thread CPU scheduling/affinity settings;
    // as of ZLINK 4.2.3 this has an effect only on POSIX systems (nothing happens on Windows, but still it should return success):
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (get_test_context (), ZLINK_THREAD_SCHED_POLICY, TEST_POLICY));
    TEST_ASSERT_EQUAL_INT (TEST_POLICY,
                           zlink_ctx_get (get_test_context (), ZLINK_THREAD_SCHED_POLICY, NULL));

    // test priority:

    // in theory SCHED_OTHER supports only the static priority 0 but quoting the docs
    //     http://man7.org/linux/man-pages/man7/sched.7.html
    // "The thread to run is chosen from the static priority 0 list based on
    // a dynamic priority that is determined only inside this list.  The
    // dynamic priority is based on the nice value [...]
    // The nice value can be modified using nice(2), setpriority(2), or sched_setattr(2)."
    // ZLINK will internally use nice(2) to set the nice value when using SCHED_OTHER.
    // However changing the nice value of a process requires appropriate permissions...
    // check that the current effective user is able to do that:
    if (is_allowed_to_raise_priority ()) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_ctx_set (get_test_context (), ZLINK_THREAD_PRIORITY,
                         1 /* any positive value different than the default will be ok */));
    }


    // test affinity:

    // this should result in background threads being placed only on the
    // first CPU available on this system; try experimenting with other values
    // (e.g., 5 to use CPU index 5) and use "top -H" or "taskset -pc" to see the result

    int cpus_add[] = {0, 1};
    for (unsigned int idx = 0; idx < sizeof (cpus_add) / sizeof (cpus_add[0]); idx++) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_ctx_set (get_test_context (), ZLINK_THREAD_AFFINITY_CPU_ADD, cpus_add[idx]));
    }

    // you can also remove CPUs from list of affinities:
    int cpus_remove[] = {1};
    for (unsigned int idx = 0; idx < sizeof (cpus_remove) / sizeof (cpus_remove[0]); idx++) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_ctx_set (get_test_context (), ZLINK_THREAD_AFFINITY_CPU_REMOVE, cpus_remove[idx]));
    }


    const char thread_name_prefix[] = "zlink-worker";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (get_test_context (), ZLINK_THREAD_NAME_PREFIX,
                          thread_name_prefix, sizeof (thread_name_prefix)));

    char read_thread_name_prefix[sizeof (thread_name_prefix)] = {};
    size_t read_thread_name_prefix_size = sizeof (read_thread_name_prefix);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_get_data (get_test_context (), ZLINK_THREAD_NAME_PREFIX,
                          read_thread_name_prefix, &read_thread_name_prefix_size));
    TEST_ASSERT_EQUAL_UINT (sizeof (thread_name_prefix), read_thread_name_prefix_size);
    TEST_ASSERT_EQUAL_STRING (thread_name_prefix, read_thread_name_prefix);

    const char non_terminated_prefix[] = {'b', 'a', 'd'};
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set_data (get_test_context (), ZLINK_THREAD_NAME_PREFIX,
                          non_terminated_prefix,
                          sizeof (non_terminated_prefix)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set (get_test_context (), ZLINK_THREAD_NAME_PREFIX, 1234));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    zlink_config_result_t get_error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT (-1, zlink_ctx_get (get_test_context (), ZLINK_THREAD_NAME_PREFIX,
                                               &get_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, get_error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

void test_ctx_zero_copy ()
{
#ifdef ZLINK_ZERO_COPY_RECV
    int zero_copy;
    // Default value is 1.
    zero_copy = zlink_ctx_get (get_test_context (), ZLINK_ZERO_COPY_RECV, NULL);
    TEST_ASSERT_EQUAL_INT (1, zero_copy);

    // Test we can set it to 0.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_ZERO_COPY_RECV, 0));
    zero_copy = zlink_ctx_get (get_test_context (), ZLINK_ZERO_COPY_RECV, NULL);
    TEST_ASSERT_EQUAL_INT (0, zero_copy);

    // Create a TCP socket pair using the context and test that messages can be
    // received. Note that inproc sockets cannot be used for this test.
    void *pull = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pull, endpoint, sizeof endpoint);

    void *push = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (push, endpoint));

    const char *small_str = "abcd";
    const char *large_str = "01234567890123456789012345678901234567890123456789";

    send_string_expect_success (push, small_str, 0);
    send_string_expect_success (push, large_str, 0);

    recv_string_expect_success (pull, small_str, 0);
    recv_string_expect_success (pull, large_str, 0);

    // Clean up.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (push));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (pull));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_ZERO_COPY_RECV, 1));
    TEST_ASSERT_EQUAL_INT (1, zlink_ctx_get (get_test_context (), ZLINK_ZERO_COPY_RECV, NULL));
#endif
}

void test_ctx_option_max_sockets ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_MAX_SOCKETS_DFLT,
                           zlink_ctx_get (get_test_context (), ZLINK_MAX_SOCKETS, NULL));
}

void test_ctx_option_socket_limit ()
{
#if defined(ZLINK_USE_SELECT)
    TEST_ASSERT_EQUAL_INT (FD_SETSIZE - 1,
                           zlink_ctx_get (get_test_context (), ZLINK_SOCKET_LIMIT, NULL));
#elif defined(ZLINK_USE_POLL) || defined(ZLINK_USE_EPOLL) || defined(ZLINK_USE_DEVPOLL)            \
  || defined(ZLINK_USE_KQUEUE)
    TEST_ASSERT_EQUAL_INT (65535, zlink_ctx_get (get_test_context (), ZLINK_SOCKET_LIMIT, NULL));
#endif
}

void test_ctx_option_io_threads ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_IO_THREADS_DFLT,
                           zlink_ctx_get (get_test_context (), ZLINK_IO_THREADS, NULL));
}

void test_ctx_option_msg_t_size ()
{
#if defined(ZLINK_MSG_T_SIZE)
    TEST_ASSERT_EQUAL_INT (sizeof (zlink_msg_t),
                           zlink_ctx_get (get_test_context (), ZLINK_MSG_T_SIZE, NULL));
#endif
}

void test_ctx_option_blocky ()
{
    TEST_ASSERT_EQUAL_INT (1, zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_BLOCKY, NULL));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    int value;
    size_t optsize = sizeof (int);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LINGER, &value, &optsize));
    TEST_ASSERT_EQUAL_INT (-1, value);
    test_context_socket_close (router);

#if WAIT_FOR_BACKGROUND_THREAD_INSPECTION
    // this is useful when you want to use an external tool (like top or taskset) to view
    // properties of the background threads
    printf ("Sleeping for 100sec. You can now use 'top -H -p $(pgrep -f "
            "test_ctx_options)' and 'taskset -pc <ZLINK background thread PID>' "
            "to view ZLINK background thread properties.\n");
    sleep (100);
#endif

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_BLOCKY, false));
    TEST_ASSERT_EQUAL_INT (0, TEST_ASSERT_SUCCESS_ERRNO (
                                (zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_BLOCKY, NULL))));
    router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LINGER, &value, &optsize));
    TEST_ASSERT_EQUAL_INT (0, value);
    test_context_socket_close (router);
}

void test_ctx_option_auto_hwm_defaults ()
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CTX_AUTO_HWM_ENABLE_DFLT,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CTX_AUTO_HWM_PROFILE_DFLT,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, NULL));
    TEST_ASSERT_EQUAL_UINT64 (
      ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT,
      get_u64_context_option (
        get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES));
    TEST_ASSERT_EQUAL_UINT64 (
      ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT,
      get_u64_context_option (
        get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES));
    TEST_ASSERT_EQUAL_UINT64 (
      ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT,
      get_u64_context_option (
        get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES));
}

void test_ctx_option_auto_hwm_round_trip ()
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0));
    TEST_ASSERT_EQUAL_INT (
      0, zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, NULL));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                                              ZLINK_AUTO_HWM_PROFILE_COMPACT));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_AUTO_HWM_PROFILE_COMPACT,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                                              ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                                              ZLINK_AUTO_HWM_PROFILE_THROUGHPUT));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_AUTO_HWM_PROFILE_THROUGHPUT,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, NULL));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, 999));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_AUTO_HWM_PROFILE_THROUGHPUT,
      zlink_ctx_get (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE, NULL));
}

static zlink_auto_hwm_budget_snapshot_t read_auto_hwm_budget_snapshot (void *ctx_)
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (ctx_, &snapshot));
    return snapshot;
}

void test_ctx_option_auto_hwm_memory_budget_round_trip_and_snapshot ()
{
    void *ctx = get_test_context ();
    const uint64_t memory_limit = 16ull * 1024ull * 1024ull;
    const uint64_t runtime_memory_limit = 8ull * 1024ull * 1024ull;
    const uint64_t core_budget = 4ull * 1024ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES,
                          &memory_limit, sizeof (memory_limit)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES,
                          &runtime_memory_limit, sizeof (runtime_memory_limit)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    TEST_ASSERT_EQUAL_UINT64 (
      memory_limit,
      get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES));
    TEST_ASSERT_EQUAL_UINT64 (
      runtime_memory_limit,
      get_u64_context_option (
        ctx, ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES));
    TEST_ASSERT_EQUAL_UINT64 (
      core_budget,
      get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES));

    zlink_auto_hwm_budget_snapshot_t snapshot = read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (memory_limit, snapshot.configured_memory_limit_bytes);
    TEST_ASSERT_EQUAL_UINT64 (runtime_memory_limit, snapshot.runtime_memory_limit_bytes);
    TEST_ASSERT_EQUAL_UINT64 (memory_limit, snapshot.resolved_memory_limit_bytes);
    TEST_ASSERT_EQUAL_UINT64 (core_budget, snapshot.configured_core_budget_bytes);
    TEST_ASSERT_EQUAL_UINT64 (core_budget, snapshot.effective_core_budget_bytes);
    TEST_ASSERT_TRUE ((snapshot.flags
                       & ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE)
                      != 0);
}

void test_ctx_option_auto_hwm_budget_snapshot_abi_and_reset ()
{
    void *ctx = get_test_context ();
    const uint64_t core_budget = 2ull * 1024ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    zlink_auto_hwm_budget_snapshot_t before = read_auto_hwm_budget_snapshot (ctx);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    zlink_auto_hwm_budget_snapshot_t after = read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (before.measurement_epoch + 1,
                              after.measurement_epoch);
    TEST_ASSERT_EQUAL_UINT64 (before.budget_generation,
                              after.budget_generation);
    TEST_ASSERT_EQUAL_UINT64 (before.effective_core_budget_bytes,
                              after.effective_core_budget_bytes);

    zlink_auto_hwm_budget_snapshot_t invalid;
    memset (&invalid, 0, sizeof (invalid));
    invalid.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1 + 1;
    invalid.struct_size = sizeof (invalid);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_ctx_get_auto_hwm_budget_snapshot (ctx, &invalid));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
}

void test_auto_hwm_metrics_reset_clears_pipe_oversize_counters ()
{
    void *ctx = get_test_context ();
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t endpoint_hwm = 128;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://auto-hwm-reset-oversize"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://auto-hwm-reset-oversize"));

    std::vector<char> payload (4096, 'o');
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (payload.size ()),
      zlink_send (sender, payload.data (), payload.size (), 0));

    const zlink_auto_hwm_budget_snapshot_t before =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_GREATER_THAN_UINT64 (0, before.oversize_admission_count);
    TEST_ASSERT_GREATER_THAN_UINT64 (
      endpoint_hwm, before.largest_oversize_message_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t after =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (before.measurement_epoch + 1,
                              after.measurement_epoch);
    TEST_ASSERT_EQUAL_UINT64 (0, after.oversize_admission_count);
    TEST_ASSERT_EQUAL_UINT64 (0, after.largest_oversize_message_bytes);
    TEST_ASSERT_EQUAL_UINT64 (before.current_accounted_bytes,
                              after.current_accounted_bytes);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_auto_hwm_blocked_ratio_counts_only_first_physical_attempt ()
{
    void *ctx = get_test_context ();
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const size_t message_size = 64;
    const uint64_t physical_hwm =
      8u * (message_size + sizeof (zlink_msg_t));
    const uint64_t endpoint_hwm = physical_hwm / 2;
    const int send_timeout_ms = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://auto-hwm-blocked-ratio"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://auto-hwm-blocked-ratio"));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    std::vector<char> oversized_more (
      static_cast<size_t> (physical_hwm), 'm');
    TEST_ASSERT_FAILURE_ERRNO (
      EAGAIN,
      zlink_send (sender, oversized_more.data (), oversized_more.size (),
                  ZLINK_DONTWAIT | ZLINK_SNDMORE));
    TEST_ASSERT_EQUAL_UINT32 (
      1000000, read_auto_hwm_budget_snapshot (ctx).blocked_ratio_ppm);

    char payload[message_size];
    memset (payload, 'b', sizeof (payload));
    int queued = 0;
    while (zlink_send (sender, payload, sizeof (payload), ZLINK_DONTWAIT)
           == static_cast<int> (sizeof (payload)))
        ++queued;
    TEST_ASSERT_GREATER_THAN_INT (0, queued);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t before =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT32 (0, before.blocked_ratio_ppm);

    std::future<int> blocked_send =
      std::async (std::launch::async, [&] () {
          return zlink_send (sender, payload, sizeof (payload), 0);
      });

    zlink_auto_hwm_budget_snapshot_t blocked = before;
    const std::chrono::steady_clock::time_point observed_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (blocked.blocked_ratio_ppm != 1000000
           && std::chrono::steady_clock::now () < observed_deadline) {
        msleep (1);
        blocked = read_auto_hwm_budget_snapshot (ctx);
    }
    TEST_ASSERT_EQUAL_UINT32 (1000000, blocked.blocked_ratio_ppm);

    char received[message_size];
    for (int i = 0; i < queued
                    && blocked_send.wait_for (std::chrono::milliseconds (0))
                         != std::future_status::ready;
         ++i) {
        TEST_ASSERT_EQUAL_INT (
          static_cast<int> (sizeof (received)),
          zlink_recv (receiver, received, sizeof (received), 0));
        (void) blocked_send.wait_for (std::chrono::milliseconds (20));
    }
    const std::future_status resumed =
      blocked_send.wait_for (std::chrono::seconds (1));
    TEST_ASSERT_EQUAL_INT (std::future_status::ready, resumed);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload)),
                           blocked_send.get ());

    const zlink_auto_hwm_budget_snapshot_t after =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT32 (1000000, after.blocked_ratio_ppm);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t reset =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (after.measurement_epoch + 1,
                              reset.measurement_epoch);
    TEST_ASSERT_EQUAL_UINT32 (0, reset.blocked_ratio_ppm);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_auto_hwm_applied_limit_blocks_and_resumes_after_drain ()
{
    void *ctx = get_test_context ();
    const uint64_t core_budget = 256ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                     ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));

    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://auto-hwm-applied-backpressure"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://auto-hwm-applied-backpressure"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));

    const zlink_auto_hwm_budget_snapshot_t applied =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (2, applied.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, applied.manual_reserved_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (core_budget,
                              applied.total_planned_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (core_budget,
                              applied.total_applied_hwm_bytes);

    const size_t message_size = 4096;
    char payload[message_size];
    memset (payload, 'a', sizeof (payload));
    int queued = 0;
    while (queued < 1024
           && zlink_send (sender, payload, sizeof (payload), ZLINK_DONTWAIT)
                == static_cast<int> (sizeof (payload)))
        ++queued;
    TEST_ASSERT_GREATER_THAN_INT (0, queued);
    TEST_ASSERT_LESS_THAN_INT (1024, queued);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    std::future<int> blocked_send =
      std::async (std::launch::async, [&] () {
          return zlink_send (sender, payload, sizeof (payload), 0);
      });

    char received[message_size];
    for (int i = 0;
         i < queued
         && blocked_send.wait_for (std::chrono::milliseconds (0))
              != std::future_status::ready;
         ++i) {
        TEST_ASSERT_EQUAL_INT (
          static_cast<int> (sizeof (received)),
          zlink_recv (receiver, received, sizeof (received), 0));
        (void) blocked_send.wait_for (std::chrono::milliseconds (20));
    }
    TEST_ASSERT_EQUAL_INT (
      std::future_status::ready,
      blocked_send.wait_for (std::chrono::seconds (1)));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload)),
                           blocked_send.get ());

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_auto_hwm_physical_queue_registry_counts_inproc_pair_once ()
{
    void *ctx = get_test_context ();
    const uint64_t core_budget = 256ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                     ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://auto-hwm-physical-queue-registry"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://auto-hwm-physical-queue-registry"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));

    const zlink_auto_hwm_budget_snapshot_t snapshot =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      2, snapshot.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot.active_send_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot.active_receive_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (core_budget,
                              snapshot.total_planned_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (core_budget,
                              snapshot.total_applied_hwm_bytes);

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

void test_auto_hwm_inproc_manual_endpoint_resolution_counts_queue_once ()
{
    void *ctx = get_test_context ();
    const uint64_t core_budget = 512ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t writer_cap = 256ull * 1024ull;
    const uint64_t reader_cap = 64ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &writer_cap,
                        sizeof (writer_cap)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &reader_cap,
                        sizeof (reader_cap)));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://auto-hwm-manual-resolution"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://auto-hwm-manual-resolution"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));

    const zlink_auto_hwm_budget_snapshot_t snapshot =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (reader_cap,
                              snapshot.manual_reserved_hwm_bytes);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64 (core_budget,
                                      snapshot.total_planned_hwm_bytes);

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

void test_auto_hwm_inproc_atomic_minimum_reservation_preserves_pending_connection ()
{
    void *ctx = get_test_context ();
    const uint64_t core_budget = 256ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                     ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &core_budget, sizeof (core_budget)));

    const char *endpoint = "inproc://auto-hwm-atomic-minimum-reservation";
    void *first = test_context_socket (ZLINK_SOCKET_DEALER);
    void *rejected = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (first, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (rejected, "D2", 2));

    // The first connect remains pending until bind. Shrinking below its
    // reservation preserves that connection but rejects the next attach
    // before publishing any pipe state.
    const zlink_auto_hwm_budget_snapshot_t before_attach =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (first, endpoint));
    const zlink_auto_hwm_budget_snapshot_t after_first_attach =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_GREATER_THAN_UINT64 (before_attach.budget_generation,
                                     after_first_attach.budget_generation);
    const uint64_t reduced_budget = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &reduced_budget, sizeof (reduced_budget)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INTERNAL_ERROR,
                           zlink_connect (rejected, endpoint));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);

    zlink_auto_hwm_budget_snapshot_t before_bind =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (2,
                              before_bind.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      2, before_bind.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (after_first_attach.budget_generation,
                              before_bind.budget_generation);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    send_string_expect_success (first, "still-connected", 0);
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "still-connected", 0);

    test_context_socket_close_zero_linger (rejected);
    test_context_socket_close_zero_linger (first);
    test_context_socket_close_zero_linger (router);
}

void test_removed_auto_hwm_message_unit_options_are_unknown ()
{
    const uint64_t value = 4096;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set_data (get_test_context (),
                          static_cast<zlink_ctx_option_t> (18), &value,
                          sizeof (value)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_option (router, static_cast<zlink_option_t> (0x3034),
                        &value, sizeof (value)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    uint64_t read_value = 0;
    size_t read_value_size = sizeof (read_value);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_get_option (router, static_cast<zlink_option_t> (0x3034),
                        &read_value, &read_value_size));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    test_context_socket_close (router);
}

static void read_socket_auto_hwm_snapshot (void *socket_, zlink_monitor_status_t *out_)
{
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = 0;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    TEST_ASSERT_NOT_NULL (monitor);
    memset (out_, 0, sizeof (*out_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_status (monitor, out_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
}

void test_socket_option_auto_hwm_buffer_options_do_not_change_snapshot_contract ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);

    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (router, &status);
    const uint64_t initial_sndhwm = status.auto_hwm_applied_sndhwm_bytes;
    const uint64_t initial_rcvhwm = status.auto_hwm_applied_rcvhwm_bytes;

    const int sndbuf = 1048576;
    const int rcvbuf = 2097152;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDBUF, &sndbuf, sizeof (sndbuf)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVBUF, &rcvbuf, sizeof (rcvbuf)));

    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (initial_sndhwm, status.auto_hwm_applied_sndhwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (initial_rcvhwm, status.auto_hwm_applied_rcvhwm_bytes);

    test_context_socket_close (router);
}

void test_socket_monitor_hwm_bytes_are_applied_without_conversion ()
{
    void *ctx = get_test_context ();
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const zlink_auto_hwm_budget_snapshot_t before =
      read_auto_hwm_budget_snapshot (ctx);

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    const uint64_t monitor_hwm_bytes = 12345;
    opts.monitor_hwm_bytes = monitor_hwm_bytes;
    void *monitor = zlink_socket_monitor_open (router, &opts);
    TEST_ASSERT_NOT_NULL (monitor);

    uint64_t value = 0;
    size_t value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (monitor, ZLINK_OPT_SNDHWM, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (monitor_hwm_bytes, value);
    value = 0;
    value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (monitor, ZLINK_OPT_RCVHWM, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (monitor_hwm_bytes, value);

    const zlink_auto_hwm_budget_snapshot_t opened =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (before.active_directional_queue_count,
                              opened.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      before.active_completion_directional_queue_count,
      opened.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (monitor_hwm_bytes * 2,
                              opened.monitor_queue_applied_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      opened.total_applied_hwm_bytes
        + opened.monitor_queue_applied_hwm_bytes,
      opened.total_instance_applied_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      opened.total_messaging_accounted_bytes
        + opened.monitor_queue_accounted_bytes,
      opened.total_instance_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t reset =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (opened.measurement_epoch + 1,
                              reset.measurement_epoch);
    TEST_ASSERT_EQUAL_UINT64 (opened.monitor_queue_applied_hwm_bytes,
                              reset.monitor_queue_applied_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (opened.monitor_queue_accounted_bytes,
                              reset.monitor_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (reset.current_accounted_bytes,
                              reset.peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (reset.completion_current_accounted_bytes,
                              reset.completion_peak_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close (router);
}

void test_socket_option_manual_hwm_overrides_auto_hwm_recalculation ()
{
    void *ctx = get_test_context ();
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_PROFILE, ZLINK_AUTO_HWM_PROFILE_BALANCED));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);

    const uint64_t manual_sndhwm = 77;
    const uint64_t manual_rcvhwm = 88;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &manual_sndhwm, sizeof (manual_sndhwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &manual_rcvhwm, sizeof (manual_rcvhwm)));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_PROFILE, ZLINK_AUTO_HWM_PROFILE_THROUGHPUT));
    const uint64_t core_budget = 4ull * 1024ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES, &core_budget,
      sizeof (core_budget)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));

    uint64_t value = 0;
    size_t value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_SNDHWM, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (manual_sndhwm, value);
    value = 0;
    value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_RCVHWM, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (manual_rcvhwm, value);

    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (manual_sndhwm, status.auto_hwm_applied_sndhwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (manual_rcvhwm, status.auto_hwm_applied_rcvhwm_bytes);

    test_context_socket_close (router);
}

void test_ctx_option_invalid ()
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set (get_test_context (), static_cast<zlink_ctx_option_t> (-1), 0));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_ctx_get (get_test_context (), static_cast<zlink_ctx_option_t> (-1), NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_ctx_option_max_sockets);
    RUN_TEST (test_ctx_option_socket_limit);
    RUN_TEST (test_ctx_option_io_threads);
    RUN_TEST (test_ctx_option_msg_t_size);
    RUN_TEST (test_ctx_thread_opts);
    RUN_TEST (test_ctx_zero_copy);
    RUN_TEST (test_ctx_option_blocky);
    RUN_TEST (test_ctx_option_auto_hwm_defaults);
    RUN_TEST (test_ctx_option_auto_hwm_round_trip);
    RUN_TEST (test_ctx_option_auto_hwm_memory_budget_round_trip_and_snapshot);
    RUN_TEST (test_ctx_option_auto_hwm_budget_snapshot_abi_and_reset);
    RUN_TEST (test_auto_hwm_metrics_reset_clears_pipe_oversize_counters);
    RUN_TEST (test_auto_hwm_blocked_ratio_counts_only_first_physical_attempt);
    RUN_TEST (test_auto_hwm_applied_limit_blocks_and_resumes_after_drain);
    RUN_TEST (test_auto_hwm_physical_queue_registry_counts_inproc_pair_once);
    RUN_TEST (test_auto_hwm_inproc_manual_endpoint_resolution_counts_queue_once);
    RUN_TEST (test_auto_hwm_inproc_atomic_minimum_reservation_preserves_pending_connection);
    RUN_TEST (test_removed_auto_hwm_message_unit_options_are_unknown);
    RUN_TEST (test_socket_option_auto_hwm_buffer_options_do_not_change_snapshot_contract);
    RUN_TEST (test_socket_monitor_hwm_bytes_are_applied_without_conversion);
    RUN_TEST (test_socket_option_manual_hwm_overrides_auto_hwm_recalculation);
    RUN_TEST (test_ctx_option_invalid);
    return UNITY_END ();
}
