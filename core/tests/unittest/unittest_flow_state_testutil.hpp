/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_UNITTEST_FLOW_STATE_TESTUTIL_HPP_INCLUDED
#define ZLINK_UNITTEST_FLOW_STATE_TESTUTIL_HPP_INCLUDED

#include "../testutil.hpp"
#include "../testutil_unity.hpp"
#include "../../src/runtime/core/internal_defs.hpp"
#include "../../src/runtime/core/recv_internal.hpp"
#include "../../src/runtime/core/send_internal.hpp"
#include "../../src/api/socket/socket_api_internal.hpp"
#include <vector>

// Raw Core frame access belongs exclusively to the internal tests. Public
// part receives expose the routing identity separately from the payload.
inline int flow_internal_send (void *socket_, const void *data_, size_t size_,
                               int flags_)
{
    zlink_msg_t part;
    if (zlink_msg_init_size (&part, size_) != ZLINK_CONFIG_OK)
        return -1;
    if (size_)
        memcpy (zlink_msg_data (&part), data_, size_);
    socket_handle_t handle = as_socket_handle (socket_);
    const int rc = handle.socket
                     ? zlink::send_msg_internal (handle.socket, &part, flags_)
                     : -1;
    const int saved_errno = errno;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    errno = saved_errno;
    return rc;
}

inline int flow_internal_recv (void *socket_, void *data_, size_t size_, int flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    return handle.socket
             ? zlink::recv_buffer_internal (handle.socket, data_, size_, flags_)
             : -1;
}

inline void flow_internal_send_string (void *socket_, const char *text_,
                                      int flags_)
{
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (text_)),
                           flow_internal_send (socket_, text_, strlen (text_),
                                               flags_));
}

inline void flow_internal_recv_string (void *socket_, const char *text_,
                                      int flags_)
{
    char buffer[4096];
    const int size = flow_internal_recv (socket_, buffer, sizeof (buffer), flags_);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (text_)), size);
    TEST_ASSERT_EQUAL_MEMORY (text_, buffer, strlen (text_));
}


// Pull public monitor records without adding a test-owned receiver thread.
// The Core monitor still owns asynchronous command and event delivery; the
// existing observation windows synchronize with that owner.
struct flow_unit_monitor_probe_t
{
    flow_unit_monitor_probe_t () : monitor (NULL) {}
    void *monitor;
    std::vector<zlink_monitor_event_t> records;
};

inline void *open_flow_unit_monitor_probe (void *socket_, uint32_t events_,
                                           flow_unit_monitor_probe_t *probe_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = events_;
    probe_->monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (probe_->monitor);
    probe_->records.clear ();
    return probe_->monitor;
}

inline void close_flow_unit_monitor_probe (void **monitor_,
                                           flow_unit_monitor_probe_t *probe_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (monitor_));
    probe_->monitor = NULL;
}

inline int flow_unit_monitor_count (flow_unit_monitor_probe_t *probe_)
{
    for (;;) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t result = zlink_socket_monitor_recv (
          probe_->monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
        probe_->records.push_back (event);
    }
    return static_cast<int> (probe_->records.size ());
}

inline bool flow_unit_monitor_has_count (flow_unit_monitor_probe_t *probe_,
                                         int expected_, int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    do {
        if (flow_unit_monitor_count (probe_) >= expected_)
            return true;
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
    return false;
}

inline bool flow_unit_monitor_has_no_additional (
  flow_unit_monitor_probe_t *probe_, int baseline_, int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    do {
        if (flow_unit_monitor_count (probe_) != baseline_)
            return false;
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
    return true;
}

inline zlink_monitor_event_t flow_unit_monitor_record_at (
  flow_unit_monitor_probe_t *probe_, int index_)
{
    TEST_ASSERT_GREATER_THAN_INT (index_, flow_unit_monitor_count (probe_));
    return probe_->records[static_cast<size_t> (index_)];
}

inline uint64_t flow_unit_monitor_event_at (flow_unit_monitor_probe_t *probe_,
                                           int index_)
{
    return flow_unit_monitor_record_at (probe_, index_).event;
}

#endif
