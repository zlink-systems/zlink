/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil_monitoring.hpp"
#include "../src/api/socket/socket_api_internal.hpp"
#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <stdlib.h>
#include <string.h>

static void record_monitor_probe_event (const zlink_monitor_event_t *event_, void *userdata_);

namespace
{
std::mutex g_active_monitor_probe_sync;
test_monitor_probe_t *g_active_monitor_probe = NULL;

void activate_test_monitor_probe (test_monitor_probe_t *probe_)
{
    std::lock_guard<std::mutex> lock (g_active_monitor_probe_sync);
    g_active_monitor_probe = probe_;
}

void deactivate_test_monitor_probe (test_monitor_probe_t *probe_)
{
    std::lock_guard<std::mutex> lock (g_active_monitor_probe_sync);
    if (g_active_monitor_probe == probe_)
        g_active_monitor_probe = NULL;
}

void record_active_test_monitor_event (const zlink_monitor_event_t *event_, void *)
{
    std::lock_guard<std::mutex> lock (g_active_monitor_probe_sync);
    record_monitor_probe_event (event_, g_active_monitor_probe);
}
}

static int wait_monitor_readable (void *monitor_, int flags_, long timeout_ms_)
{
    if ((flags_ & ZLINK_DONTWAIT) != 0)
        timeout_ms_ = 0;

    zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
    const int rc = zlink_poll (&item, 1, timeout_ms_, NULL);
    if (rc <= 0 || (item.revents & ZLINK_POLLIN) == 0) {
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int recv_monitor_event_from_socket (void *monitor_, zlink_monitor_event_t *event_, int flags_)
{
    if (!monitor_ || !event_) {
        errno = EINVAL;
        return -1;
    }

    if (wait_monitor_readable (monitor_, flags_, 0) != 0)
        return -1;

    return zlink_socket_monitor_recv (monitor_, event_, static_cast<zlink_recv_flags_t> (flags_));
}

//  Read one event off the monitor socket; return value and address
//  by reference, if not null, and event number by value. Returns -1
//  in case of error.
static int get_monitor_event_internal (void *monitor_, int *value_, char **address_, int recv_flag_)
{
    zlink_monitor_event_t event;
    if (recv_monitor_event_from_socket (monitor_, &event, recv_flag_) == -1) {
        TEST_ASSERT_FAILURE_ERRNO (EAGAIN, -1);
        return -1;
    }

    if (value_)
        *value_ = static_cast<int> (event.value);

    if (address_) {
        const char *addr = event.remote_addr[0] ? event.remote_addr : event.local_addr;
        const size_t len = strlen (addr);
        *address_ = static_cast<char *> (malloc (len + 1));
        memcpy (*address_, addr, len);
        (*address_)[len] = '\0';
    }

    return static_cast<int> (event.event);
}

int get_monitor_event_with_timeout (void *monitor_, int *value_, char **address_, int timeout_)
{
    int res;
    if (timeout_ == -1) {
        int timeout_step = 250;
        int wait_time = 0;
        while (true) {
            if (wait_monitor_readable (monitor_, 0, timeout_step) != 0) {
                wait_time += timeout_step;
                fprintf (stderr, "Still waiting for monitor event after %i ms\n", wait_time);
                continue;
            }
            res = get_monitor_event_internal (monitor_, value_, address_, 0);
            if (res != -1)
                break;
            wait_time += timeout_step;
            fprintf (stderr, "Still waiting for monitor event after %i ms\n", wait_time);
        }
    } else {
        if (wait_monitor_readable (monitor_, 0, timeout_) != 0) {
            return -1;
        }
        res = get_monitor_event_internal (monitor_, value_, address_, 0);
    }
    return res;
}

int get_monitor_event (void *monitor_, int *value_, char **address_)
{
    return get_monitor_event_with_timeout (monitor_, value_, address_, -1);
}

void expect_monitor_event (void *monitor_, int expected_event_)
{
    TEST_ASSERT_EQUAL_HEX (expected_event_, get_monitor_event (monitor_, NULL, NULL));
}

static void print_unexpected_event (
  char *buf_, size_t buf_size_, int event_, int err_, int expected_event_, int expected_err_)
{
    snprintf (buf_, buf_size_,
              "Unexpected event: 0x%x, value = %i/0x%x (expected: 0x%x, value "
              "= %i/0x%x)\n",
              event_, err_, err_, expected_event_, expected_err_, expected_err_);
}

int expect_monitor_event_multiple (void *server_mon_,
                                   int expected_event_,
                                   int expected_err_,
                                   bool optional_)
{
    int count_of_expected_events = 0;
    int client_closed_connection = 0;
    int timeout = 250;
    int wait_time = 0;

    int event;
    int err;
    while ((event = get_monitor_event_with_timeout (server_mon_, &err, NULL, timeout)) != -1
           || !count_of_expected_events) {
        if (event == -1) {
            if (optional_)
                break;
            wait_time += timeout;
            fprintf (stderr,
                     "Still waiting for first event after %ims (expected event "
                     "%x (value %i/0x%x))\n",
                     wait_time, expected_event_, expected_err_, expected_err_);
            continue;
        }
        // ignore errors with EPIPE/ECONNRESET/ECONNABORTED, which can happen
        // ECONNRESET can happen on very slow machines, when the engine writes
        // to the peer and then tries to read the socket before the peer reads
        // ECONNABORTED happens when a client aborts a connection via RST/timeout
        if (event == ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
            && ((err == EPIPE && expected_err_ != EPIPE) || err == ECONNRESET
                || err == ECONNABORTED)) {
            fprintf (stderr,
                     "Ignored event (skipping any further events): %x (err = "
                     "%i == %s)\n",
                     event, err, zlink_strerror (err));
            client_closed_connection = 1;
            break;
        }
        if (event != expected_event_ || (-1 != expected_err_ && err != expected_err_)) {
            char buf[256];
            print_unexpected_event (buf, sizeof buf, event, err, expected_event_, expected_err_);
            TEST_FAIL_MESSAGE (buf);
        }
        ++count_of_expected_events;
    }
    TEST_ASSERT_TRUE (optional_ || count_of_expected_events > 0 || client_closed_connection);

    return count_of_expected_events;
}

static int64_t get_monitor_event_internal_v2 (
  void *monitor_, uint64_t **value_, char **local_address_, char **remote_address_, int recv_flag_)
{
    zlink_monitor_event_t event;
    if (recv_monitor_event_from_socket (monitor_, &event, recv_flag_) == -1) {
        TEST_ASSERT_FAILURE_ERRNO (EAGAIN, -1);
        return -1;
    }

    if (value_) {
        *value_ = static_cast<uint64_t *> (malloc (sizeof (uint64_t)));
        TEST_ASSERT_NOT_NULL (*value_);
        (*value_)[0] = event.value;
    }

    if (local_address_) {
        const size_t len = strlen (event.local_addr);
        *local_address_ = static_cast<char *> (malloc (len + 1));
        memcpy (*local_address_, event.local_addr, len);
        (*local_address_)[len] = '\0';
    }

    if (remote_address_) {
        const size_t len = strlen (event.remote_addr);
        *remote_address_ = static_cast<char *> (malloc (len + 1));
        memcpy (*remote_address_, event.remote_addr, len);
        (*remote_address_)[len] = '\0';
    }

    return static_cast<int64_t> (event.event);
}

static int64_t get_monitor_event_with_timeout_v2 (
  void *monitor_, uint64_t **value_, char **local_address_, char **remote_address_, int timeout_)
{
    int64_t res;
    if (timeout_ == -1) {
        int timeout_step = 250;
        int wait_time = 0;
        while (true) {
            socket_handle_t handle = as_socket_handle (monitor_);
            if (!handle.socket
                || zlink::wait_socket_events_internal (handle.socket, 1,
                                                       timeout_step) <= 0) {
                wait_time += timeout_step;
                fprintf (stderr, "Still waiting for monitor event after %i ms\n", wait_time);
                continue;
            }
            res =
              get_monitor_event_internal_v2 (monitor_, value_, local_address_, remote_address_, 0);
            if (res != -1)
                break;
            wait_time += timeout_step;
            fprintf (stderr, "Still waiting for monitor event after %i ms\n", wait_time);
        }
    } else {
        socket_handle_t handle = as_socket_handle (monitor_);
        if (!handle.socket
            || zlink::wait_socket_events_internal (handle.socket, 1, timeout_) <= 0) {
            errno = EAGAIN;
            return -1;
        }
        res = get_monitor_event_internal_v2 (monitor_, value_, local_address_, remote_address_, 0);
    }
    return res;
}

int64_t get_monitor_event_v2 (void *monitor_,
                              uint64_t **value_,
                              char **local_address_,
                              char **remote_address_)
{
    return get_monitor_event_with_timeout_v2 (monitor_, value_, local_address_, remote_address_,
                                              -1);
}

void expect_monitor_event_v2 (void *monitor_,
                              int64_t expected_event_,
                              const char *expected_local_address_,
                              const char *expected_remote_address_)
{
    char *local_address = NULL;
    char *remote_address = NULL;
    int64_t event =
      get_monitor_event_v2 (monitor_, NULL, expected_local_address_ ? &local_address : NULL,
                            expected_remote_address_ ? &remote_address : NULL);
    bool failed = false;
    char buf[256];
    char *pos = buf;
    if (event != expected_event_) {
        pos += snprintf (pos, sizeof buf - (pos - buf),
                         "Expected monitor event %llx, but received %llx\n",
                         static_cast<long long> (expected_event_), static_cast<long long> (event));
        failed = true;
    }
    if (expected_local_address_ && 0 != strcmp (local_address, expected_local_address_)) {
        pos +=
          snprintf (pos, sizeof buf - (pos - buf), "Expected local address %s, but received %s\n",
                    expected_local_address_, local_address);
    }
    if (expected_remote_address_ && 0 != strcmp (remote_address, expected_remote_address_)) {
        snprintf (pos, sizeof buf - (pos - buf), "Expected remote address %s, but received %s\n",
                  expected_remote_address_, remote_address);
    }
    free (local_address);
    free (remote_address);
    TEST_ASSERT_FALSE_MESSAGE (failed, buf);
}

static void record_monitor_probe_event (const zlink_monitor_event_t *event_, void *userdata_)
{
    test_monitor_probe_t *probe = static_cast<test_monitor_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    std::lock_guard<std::mutex> lock (probe->sync);
    probe->events.push_back (event_->event);
    probe->records.push_back (*event_);
}

zlink_monitor_event_t test_monitor_probe_record_at (test_monitor_probe_t *probe_,
                                                    int index_)
{
    std::lock_guard<std::mutex> lock (probe_->sync);
    TEST_ASSERT_TRUE (index_ >= 0);
    TEST_ASSERT_TRUE (index_ < static_cast<int> (probe_->records.size ()));
    return probe_->records[static_cast<size_t> (index_)];
}

int test_monitor_probe_count (test_monitor_probe_t *probe_)
{
    std::lock_guard<std::mutex> lock (probe_->sync);
    return static_cast<int> (probe_->events.size ());
}

uint64_t test_monitor_probe_event_at (test_monitor_probe_t *probe_, int index_)
{
    std::lock_guard<std::mutex> lock (probe_->sync);
    TEST_ASSERT_TRUE (index_ >= 0);
    TEST_ASSERT_TRUE (index_ < static_cast<int> (probe_->events.size ()));
    return probe_->events[static_cast<size_t> (index_)];
}

void *open_test_monitor_probe (void *socket_,
                               zlink_socket_monitor_event_mask_t events_,
                               test_monitor_probe_t *probe_)
{
    activate_test_monitor_probe (probe_);
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = events_;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_socket_monitor_handler (monitor, &record_active_test_monitor_event, NULL));
    return monitor;
}

void close_test_monitor_probe (void **monitor_p_, test_monitor_probe_t *probe_)
{
    deactivate_test_monitor_probe (probe_);
    if (!monitor_p_ || !*monitor_p_)
        return;

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (*monitor_p_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (monitor_p_));
}

bool test_monitor_probe_wait_count (test_monitor_probe_t *probe_, int expected_, int timeout_ms_)
{
    return zlink_test_wait_until_step (
      timeout_ms_, 10, [=] { return test_monitor_probe_count (probe_) >= expected_; });
}

bool test_monitor_probe_wait_no_additional (test_monitor_probe_t *probe_,
                                            int baseline_,
                                            int timeout_ms_)
{
    const bool saw_additional = zlink_test_wait_until_step (
      timeout_ms_, 10, [=] { return test_monitor_probe_count (probe_) > baseline_; });
    return !saw_additional && test_monitor_probe_count (probe_) == baseline_;
}

bool test_monitor_probe_wait_event_after (test_monitor_probe_t *probe_,
                                          uint64_t expected_,
                                          int start_index_,
                                          int timeout_ms_,
                                          int *found_index_out_)
{
    return zlink_test_wait_until_step (timeout_ms_, 10, [=] {
        std::lock_guard<std::mutex> lock (probe_->sync);
        const int count = static_cast<int> (probe_->events.size ());
        for (int i = start_index_; i < count; ++i) {
            if (probe_->events[static_cast<size_t> (i)] != expected_)
                continue;
            if (found_index_out_)
                *found_index_out_ = i;
            return true;
        }
        return false;
    });
}

bool wait_monitor_event_routing_id (void *monitor_,
                                    void *activity_socket_,
                                    uint64_t expected_event_,
                                    unsigned char *routing_id_out_,
                                    size_t routing_id_size_,
                                    int timeout_ms_)
{
    const int poll_slice_ms = 200;
    const int poll_timeout = timeout_ms_ > 0 ? timeout_ms_ : 10000;
    return zlink_test_wait_until_step (poll_timeout, poll_slice_ms, [=] {
        zlink_pollitem_t items[] = {
          {monitor_, 0, ZLINK_POLLIN, 0},
          {activity_socket_, 0, ZLINK_POLLIN, 0},
        };
        const int count = activity_socket_ ? 2 : 1;
        const int rc = zlink_poll (items, count, poll_slice_ms, NULL);
        if (rc <= 0 || (items[0].revents & ZLINK_POLLIN) == 0)
            return false;

        for (;;) {
            zlink_monitor_event_t event;
            if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0)
                break;
            if (event.event != expected_event_)
                continue;
            if (event.routing_id.size != routing_id_size_)
                continue;
            memcpy (routing_id_out_, event.routing_id.data, routing_id_size_);
            return true;
        }
        return false;
    });
}
