/* SPDX-License-Identifier: MPL-2.0 */

#include <limits>
#include <vector>
#include <string.h>
#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

#define WAIT_FOR_BACKGROUND_THREAD_INSPECTION (0)

static void read_socket_auto_hwm_snapshot (void *socket_, zlink_monitor_status_t *out_);

static uint64_t get_u64_socket_option (void *socket_, zlink_option_t option_)
{
    uint64_t value = UINT64_MAX;
    size_t value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (socket_, option_, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (value), value_size);
    return value;
}

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


    // test INTEGER thread name prefix:

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_THREAD_NAME_PREFIX, 1234));
    TEST_ASSERT_EQUAL_INT (1234,
                           zlink_ctx_get (get_test_context (), ZLINK_THREAD_NAME_PREFIX, NULL));
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

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull,
                              get_u64_socket_option (router, ZLINK_OPT_SNDHWM));
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull,
                              get_u64_socket_option (router, ZLINK_OPT_RCVHWM));
    test_context_socket_close (router);
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

void test_ctx_option_auto_hwm_msg_unit_round_trip_and_validation ()
{
    void *ctx = get_test_context ();
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT,
                              get_u64_context_option (
                                ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));

    const uint64_t value_64 = 64;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES,
                          &value_64, sizeof (value_64)));
    TEST_ASSERT_EQUAL_UINT64 (
      value_64, get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));

    const int negative = -1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT,
                           zlink_ctx_set (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, negative));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (
      value_64, get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));

    const uint64_t max_value = UINT64_MAX;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES,
                          &max_value, sizeof (max_value)));
    TEST_ASSERT_EQUAL_UINT64 (
      max_value, get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));

    const int value = 128;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, sizeof (value)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (
      max_value, get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));

    const uint64_t zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_UINT64 (
      0, get_u64_context_option (ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES));
}

void test_ctx_option_auto_hwm_msg_unit_applies_to_socket_snapshots ()
{
    void *ctx = get_test_context ();
    const uint64_t unit_64 = 64;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_64, sizeof (unit_64)));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (64u, status.auto_hwm_effective_message_bytes);

    const uint64_t unit_256 = 256;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_256, sizeof (unit_256)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (256u, status.auto_hwm_effective_message_bytes);

    const uint64_t unit_zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_zero, sizeof (unit_zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (4096u, status.auto_hwm_effective_message_bytes);
    test_context_socket_close (router);

    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    read_socket_auto_hwm_snapshot (stream, &status);
    TEST_ASSERT_EQUAL_UINT64 (1024u, status.auto_hwm_effective_message_bytes);
    test_context_socket_close (stream);
}

void test_ctx_option_auto_hwm_msg_unit_preserves_socket_override_priority ()
{
    void *ctx = get_test_context ();
    const uint64_t unit_64 = 64;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_64, sizeof (unit_64)));

    void *explicit_socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (explicit_socket);
    void *context_socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (context_socket);

    uint64_t override_value = 512;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (explicit_socket, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES,
                                                 &override_value, sizeof (override_value)));

    const uint64_t unit_256 = 256;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_256, sizeof (unit_256)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));

    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (explicit_socket, &status);
    TEST_ASSERT_EQUAL_UINT64 (512u, status.auto_hwm_effective_message_bytes);
    read_socket_auto_hwm_snapshot (context_socket, &status);
    TEST_ASSERT_EQUAL_UINT64 (256u, status.auto_hwm_effective_message_bytes);

    override_value = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (explicit_socket, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES,
                                                 &override_value, sizeof (override_value)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    read_socket_auto_hwm_snapshot (explicit_socket, &status);
    TEST_ASSERT_EQUAL_UINT64 (256u, status.auto_hwm_effective_message_bytes);

    const uint64_t unit_zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_zero, sizeof (unit_zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx));
    read_socket_auto_hwm_snapshot (explicit_socket, &status);
    TEST_ASSERT_EQUAL_UINT64 (4096u, status.auto_hwm_effective_message_bytes);

    test_context_socket_close (context_socket);
    test_context_socket_close (explicit_socket);
}

void test_ctx_option_auto_hwm_enabled_applies_profile ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                                              ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 1));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull,
                              get_u64_socket_option (router, ZLINK_OPT_SNDHWM));
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull,
                              get_u64_socket_option (router, ZLINK_OPT_RCVHWM));
    int buffer = -1;
    size_t buffer_size = sizeof (buffer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_SNDBUF, &buffer, &buffer_size));
    TEST_ASSERT_EQUAL_INT (-1, buffer);
    buffer = -1;
    buffer_size = sizeof (buffer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_RCVBUF, &buffer, &buffer_size));
    TEST_ASSERT_EQUAL_INT (-1, buffer);

    test_context_socket_close (router);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                                              ZLINK_AUTO_HWM_PROFILE_COMPACT));
    router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_EQUAL_UINT64 (64ull * 4096ull,
                              get_u64_socket_option (router, ZLINK_OPT_SNDHWM));
    buffer = -1;
    buffer_size = sizeof (buffer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_SNDBUF, &buffer, &buffer_size));
    TEST_ASSERT_EQUAL_INT (-1, buffer);
    buffer = -1;
    buffer_size = sizeof (buffer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_RCVBUF, &buffer, &buffer_size));
    TEST_ASSERT_EQUAL_INT (-1, buffer);

    test_context_socket_close (router);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0));
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

void test_socket_option_auto_hwm_msg_unit_round_trip ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);

    uint64_t value = UINT64_MAX;
    size_t value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (0, value);

    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (4096u, status.auto_hwm_effective_message_bytes);
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_CTX_AUTO_HWM_PROFILE_DFLT, status.auto_hwm_profile);
    TEST_ASSERT_GREATER_THAN_UINT32 (0u, status.auto_hwm_policy_class);
    TEST_ASSERT_GREATER_THAN_UINT64 (0u, status.auto_hwm_unit_budget_bytes);
    TEST_ASSERT_GREATER_THAN_UINT32 (0u, status.auto_hwm_size_cap);

    value = 8192;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, sizeof (value)));
    value = 0;
    value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (8192, value);
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (8192u, status.auto_hwm_effective_message_bytes);

    value = UINT64_MAX;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, sizeof (value)));
    value = 0;
    value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, value);
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, status.auto_hwm_effective_message_bytes);
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, status.auto_hwm_unit_budget_bytes);

    const int negative = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &negative, sizeof (negative)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    value = 0;
    value_size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, &value_size));
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, value);

    value = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, &value, sizeof (value)));
    read_socket_auto_hwm_snapshot (router, &status);
    TEST_ASSERT_EQUAL_UINT64 (4096u, status.auto_hwm_effective_message_bytes);

    test_context_socket_close (router);
}

void test_socket_option_auto_hwm_stream_default_msg_unit ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);

    zlink_monitor_status_t status;
    read_socket_auto_hwm_snapshot (stream, &status);
    TEST_ASSERT_EQUAL_UINT64 (1024u, status.auto_hwm_effective_message_bytes);

    test_context_socket_close (stream);
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
    const uint64_t unit_64 = 64;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set_data (
      ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_64, sizeof (unit_64)));
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
    RUN_TEST (test_ctx_option_auto_hwm_msg_unit_round_trip_and_validation);
    RUN_TEST (test_ctx_option_auto_hwm_msg_unit_applies_to_socket_snapshots);
    RUN_TEST (test_ctx_option_auto_hwm_msg_unit_preserves_socket_override_priority);
    RUN_TEST (test_ctx_option_auto_hwm_enabled_applies_profile);
    RUN_TEST (test_socket_option_auto_hwm_msg_unit_round_trip);
    RUN_TEST (test_socket_option_auto_hwm_stream_default_msg_unit);
    RUN_TEST (test_socket_option_auto_hwm_buffer_options_do_not_change_snapshot_contract);
    RUN_TEST (test_socket_option_manual_hwm_overrides_auto_hwm_recalculation);
    RUN_TEST (test_ctx_option_invalid);
    return UNITY_END ();
}
