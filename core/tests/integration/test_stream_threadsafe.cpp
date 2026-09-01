/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
bool selected (const char *name_)
{
    const char *value = getenv ("ZLINK_TEST_CASE");
    return !value || !*value || strcmp (value, name_) == 0;
}

#if defined(ZLINK_HAVE_WINDOWS)
void run_concurrent_send_case (int, int, bool, bool)
{
    TEST_IGNORE_MESSAGE ("raw STREAM thread-safety fixture is POSIX-only");
}
#else
int connect_raw_tcp (const char *endpoint_)
{
    char host[64] = {0};
    int port = 0;
    TEST_ASSERT_EQUAL_INT (
      2, sscanf (endpoint_, "tcp://%63[^:]:%d", host, &port));
    const int fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT_TRUE (fd >= 0);
    sockaddr_in address;
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons (static_cast<uint16_t> (port));
    TEST_ASSERT_EQUAL_INT (1, inet_pton (AF_INET, host, &address.sin_addr));
    TEST_ASSERT_EQUAL_INT (
      0, connect (fd, reinterpret_cast<const sockaddr *> (&address),
                  sizeof (address)));
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    TEST_ASSERT_EQUAL_INT (
      0, setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)));
    return fd;
}

zlink_routing_id_t discover_stream_rid (void *stream_, int fd_)
{
    const unsigned char probe = 0x7f;
    TEST_ASSERT_EQUAL_INT (1, send (fd_, &probe, sizeof (probe), 0));

    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    const zlink_routing_id_t *borrowed_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_recv_result_t rc = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < 1000 && rc == ZLINK_RECV_NO_DATA;
         ++attempt) {
        rc = zlink_recv_part (stream_, &borrowed_rid, &received, &has_more,
                              ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    TEST_ASSERT_NOT_NULL (borrowed_rid);
    TEST_ASSERT_EQUAL_UINT64 (4, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&received));
    zlink_routing_id_t rid = *borrowed_rid;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    return rid;
}

void run_concurrent_send_case (int sender_count_, int sends_per_thread_,
                               bool read_endpoint_, bool read_events_)
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    const int fd = connect_raw_tcp (endpoint);
    const zlink_routing_id_t rid = discover_stream_rid (server, fd);

    const size_t payload_size = 32;
    const size_t expected = static_cast<size_t> (sender_count_)
                            * static_cast<size_t> (sends_per_thread_)
                            * payload_size;
    std::atomic<size_t> received (0);
    std::atomic<int> read_errors (0);
    std::atomic<bool> stop_reads (false);

    std::thread raw_reader ([&] () {
        unsigned char buffer[4096];
        while (received.load (std::memory_order_acquire) < expected) {
            const ssize_t n = recv (fd, buffer, sizeof (buffer), 0);
            if (n > 0)
                received.fetch_add (static_cast<size_t> (n),
                                    std::memory_order_release);
            else if (n < 0 && errno == EINTR)
                continue;
            else {
                read_errors.fetch_add (1, std::memory_order_release);
                break;
            }
        }
    });

    std::thread runtime_reader;
    if (read_endpoint_ || read_events_) {
        runtime_reader = std::thread ([&] () {
            while (!stop_reads.load (std::memory_order_acquire)) {
                if (read_endpoint_) {
                    char current[MAX_SOCKET_STRING];
                    size_t size = sizeof (current);
                    if (zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT,
                                          current, &size)
                        != ZLINK_CONFIG_OK)
                        read_errors.fetch_add (1, std::memory_order_release);
                }
                if (read_events_) {
                    uint32_t events = 0;
                    size_t size = sizeof (events);
                    if (zlink_get_option (server, ZLINK_OPT_EVENTS, &events,
                                          &size)
                        != ZLINK_CONFIG_OK)
                        read_errors.fetch_add (1, std::memory_order_release);
                }
            }
        });
    }

    std::vector<std::thread> senders;
    for (int sender = 0; sender < sender_count_; ++sender) {
        senders.push_back (std::thread ([&, sender] () {
            for (int sequence = 0; sequence < sends_per_thread_; ++sequence) {
                zlink_msg_t part;
                TEST_ASSERT_SUCCESS_ERRNO (
                  zlink_msg_init_size (&part, payload_size));
                memset (zlink_msg_data (&part), 0x40 + sender, payload_size);
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_SUBMIT_OK,
                  zlink_send_part_rid (server, &rid, &part,
                                       ZLINK_SEND_FLAGS_NONE,
                                       ZLINK_PART_FINAL, NULL, NULL));
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
            }
        }));
    }
    for (size_t i = 0; i < senders.size (); ++i)
        senders[i].join ();
    raw_reader.join ();
    stop_reads.store (true, std::memory_order_release);
    if (runtime_reader.joinable ())
        runtime_reader.join ();

    TEST_ASSERT_EQUAL_UINT64 (expected, received.load ());
    TEST_ASSERT_EQUAL_INT (0, read_errors.load ());
    close (fd);
    test_context_socket_close_zero_linger (server);
}
#endif
}

void test_stream_send_part_rid_is_thread_safe ()
{
    run_concurrent_send_case (4, 64, false, false);
}

void test_stream_send_part_rid_high_contention_is_thread_safe ()
{
    run_concurrent_send_case (8, 32, false, false);
}

void test_stream_last_endpoint_read_is_safe_during_send ()
{
    run_concurrent_send_case (4, 64, true, false);
}

void test_stream_events_read_is_safe_during_send ()
{
    run_concurrent_send_case (4, 64, false, true);
}

void test_stream_runtime_reads_are_safe_during_send ()
{
    run_concurrent_send_case (4, 64, true, true);
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    if (selected ("test_stream_send_part_rid_is_thread_safe"))
        RUN_TEST (test_stream_send_part_rid_is_thread_safe);
    if (selected ("test_stream_send_part_rid_high_contention_is_thread_safe"))
        RUN_TEST (test_stream_send_part_rid_high_contention_is_thread_safe);
    if (selected ("test_stream_last_endpoint_read_is_safe_during_send"))
        RUN_TEST (test_stream_last_endpoint_read_is_safe_during_send);
    if (selected ("test_stream_events_read_is_safe_during_send"))
        RUN_TEST (test_stream_events_read_is_safe_during_send);
    if (selected ("test_stream_runtime_reads_are_safe_during_send"))
        RUN_TEST (test_stream_runtime_reads_are_safe_during_send);
    return UNITY_END ();
}
