/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstring>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
typedef std::chrono::steady_clock clock_type;
const int wait_ms = 3000;

zlink_routing_id_t rid (const char *text_)
{
    zlink_routing_id_t value = {};
    value.size = static_cast<uint8_t> (strlen (text_));
    memcpy (value.data, text_, value.size);
    return value;
}

void *router (const char *name_)
{
    void *socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int zero = 0;
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (socket, name_, strlen (name_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (socket, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (socket, ZLINK_OPT_RECONNECT_IVL, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (socket, ZLINK_OPT_RID_DUPLICATE_POLICY,
                                             &handover, sizeof handover));
    return socket;
}

void connect_as (void *source_, const char *peer_, const char *endpoint_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_router_option (source_,
                                                    ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                    peer_, strlen (peer_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (source_, endpoint_));
}

size_t snapshot (void *socket_, zlink_router_route_t *rows_, size_t capacity_)
{
    size_t count = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_router_routes_snapshot (socket_, rows_, capacity_, &count));
    TEST_ASSERT_TRUE (count <= capacity_);
    for (size_t i = 0; i < count; ++i)
        TEST_ASSERT_NOT_EQUAL (0, rows_[i].route_generation);
    return count;
}

uint64_t generation (void *socket_, const char *peer_)
{
    zlink_router_route_t rows[8] = {};
    const size_t count = snapshot (socket_, rows, 8);
    const zlink_routing_id_t expected = rid (peer_);
    size_t matches = 0;
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        if (rows[i].rid.size == expected.size
            && memcmp (rows[i].rid.data, expected.data, expected.size) == 0) {
            ++matches;
            value = rows[i].route_generation;
        }
    TEST_ASSERT_EQUAL_UINT64 (1, matches);
    return value;
}

void wait_route (void *socket_, const char *peer_, uint64_t old_ = 0)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    do {
        zlink_router_route_t rows[8] = {};
        const size_t count = snapshot (socket_, rows, 8);
        const zlink_routing_id_t expected = rid (peer_);
        for (size_t i = 0; i < count; ++i)
            if (rows[i].rid.size == expected.size
                && memcmp (rows[i].rid.data, expected.data, expected.size) == 0
                && rows[i].route_generation != old_)
                return;
        zlink_pollitem_t item = {socket_, 0, ZLINK_POLLROUTE, 0};
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    } while (clock_type::now () < deadline);
    TEST_FAIL_MESSAGE ("selected route did not change");
}

short poll_now (void *socket_, short mask_)
{
    zlink_pollitem_t item = {socket_, 0, mask_, 123};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    const int count = zlink_poll (&item, 1, 0, &error);
    TEST_ASSERT_TRUE (count >= 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    return item.revents;
}

void send_data (void *socket_, const char *peer_, const char *payload_)
{
    zlink_msg_t part;
    const size_t size = strlen (payload_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size));
    memcpy (zlink_msg_data (&part), payload_, size);
    const zlink_routing_id_t target = rid (peer_);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send_rid (socket_, &target, &part, 1,
                                           ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void recv_data (void *socket_, const char *peer_, const char *payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (socket_, &source, &token, &part, 1,
                                              &count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_INT (strlen (peer_), source->size);
    TEST_ASSERT_EQUAL_MEMORY (peer_, source->data, source->size);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (payload_), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&part), strlen (payload_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void test_single_data_zero_capacity_retries_same_record ()
{
    void *server = router ("S");
    void *client = router ("C");
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    connect_as (client, "S", endpoint);
    wait_route (client, "S");
    wait_route (server, "C");

    send_data (server, "C", "single");
    zlink_pollitem_t item = {client, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, &error));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);

    zlink_msg_t slot;
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUFFER_TOO_SMALL,
                           zlink_router_recv (client, &source, &token, &slot, 0,
                                              &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_router_recv_route_generation (client));

    recv_data (client, "S", "single");
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void no_data (void *socket_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_router_recv (socket_, &source, &token, &part, 1,
                                              &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_router_recv_route_generation (socket_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void test_snapshot_polling_and_recv_generation ()
{
    void *server = router ("S");
    void *client = router ("C");
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    zlink_router_route_t rows[2] = {};
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot (client, rows, 2));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_router_recv_route_generation (client));
    size_t count = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED,
                           zlink_router_routes_snapshot (pair, rows, 2, &count));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_router_recv_route_generation (pair));

    zlink_config_result_t error = ZLINK_CONFIG_OK;
    zlink_pollitem_t item = {pair, 0, ZLINK_POLLROUTE, 0};
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED, error);
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    item.events = 128;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED,
                           zlink_poller_add (poller, pair, NULL, ZLINK_POLLROUTE));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT,
                           zlink_poller_add (poller, client, NULL, 128));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, client, client, ZLINK_POLLROUTE));

    connect_as (client, "S", endpoint);
    zlink_poller_event_t event = {};
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, wait_ms, &error));
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLROUTE) != 0);
    TEST_ASSERT_EQUAL_PTR (client, event.socket);
    TEST_ASSERT_TRUE ((poll_now (client, ZLINK_POLLROUTE) & ZLINK_POLLROUTE) != 0);
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 0, &error));

    count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_BUFFER_TOO_SMALL,
                           zlink_router_routes_snapshot (client, NULL, 0, &count));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_TRUE ((poll_now (client, ZLINK_POLLROUTE) & ZLINK_POLLROUTE) != 0);
    const uint64_t selected = generation (client, "S");
    TEST_ASSERT_NOT_EQUAL (0, selected);
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLROUTE));
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 0, &error));

    void *closing = router ("X");
    void *close_poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (close_poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (close_poller, closing, NULL,
                                             ZLINK_POLLROUTE));
    test_context_socket_close_zero_linger (closing);
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (close_poller, &event, 1,
                                                 wait_ms, &error));
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLERR) != 0);
    TEST_ASSERT_EQUAL_INT (0, event.events & ZLINK_POLLROUTE);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (close_poller, closing));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&close_poller));

    wait_route (server, "C");
    send_data (server, "C", "selected");
    item = {client, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, &error));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
    recv_data (client, "S", "selected");
    TEST_ASSERT_EQUAL_UINT64 (selected,
                              zlink_router_recv_route_generation (client));
    no_data (client);

    void *second = router ("T");
    char second_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (second, second_endpoint, sizeof second_endpoint);
    connect_as (client, "T", second_endpoint);
    wait_route (client, "T");
    count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_BUFFER_TOO_SMALL,
                           zlink_router_routes_snapshot (client, rows, 1, &count));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot (client, rows, 2));
    TEST_ASSERT_TRUE (rows[0].rid.size != rows[1].rid.size
                      || memcmp (rows[0].rid.data, rows[1].rid.data,
                                 rows[0].rid.size) != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (pair);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (server);
}

void test_handover_discards_old_records_and_changes_generation ()
{
    void *old_server = router ("S");
    void *new_server = router ("S");
    void *client = router ("C");
    char old_endpoint[MAX_SOCKET_STRING];
    char new_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (old_server, old_endpoint, sizeof old_endpoint);
    bind_loopback_ipv4 (new_server, new_endpoint, sizeof new_endpoint);
    connect_as (client, "S", old_endpoint);
    wait_route (client, "S");
    wait_route (old_server, "C");
    const uint64_t first = generation (client, "S");

    zlink_msg_t old_parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&old_parts[0], 1));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&old_parts[1], 1));
    *static_cast<char *> (zlink_msg_data (&old_parts[0])) = 'o';
    *static_cast<char *> (zlink_msg_data (&old_parts[1])) = 'l';
    const zlink_routing_id_t client_rid = rid ("C");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send_rid (old_server, &client_rid, old_parts, 2,
                                           ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&old_parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&old_parts[1]));
    zlink_pollitem_t item = {client, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, &error));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUFFER_TOO_SMALL,
                           zlink_router_recv (client, &source, &token, &part, 1,
                                              &part_count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_router_recv_route_generation (client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    connect_as (client, "S", new_endpoint);
    wait_route (client, "S", first);
    const uint64_t second = generation (client, "S");
    TEST_ASSERT_NOT_EQUAL (first, second);
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLROUTE));
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLIN));
    no_data (client);

    wait_route (new_server, "C");
    send_data (new_server, "C", "new");
    recv_data (client, "S", "new");
    TEST_ASSERT_EQUAL_UINT64 (second,
                              zlink_router_recv_route_generation (client));
    no_data (client);

    // A record arriving on the retained standby must not become visible when
    // the selected pipe later disappears and that standby is promoted.
    send_data (old_server, "C", "standby");
    item = {client, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 100, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);

    // A blocking receive must continue after skipping an unselected multipart record.
    zlink_msg_t stale_parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&stale_parts[0], 1));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&stale_parts[1], 1));
    *static_cast<char *> (zlink_msg_data (&stale_parts[0])) = 's';
    *static_cast<char *> (zlink_msg_data (&stale_parts[1])) = 't';
    const zlink_routing_id_t stale_target = rid ("C");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send_rid (old_server, &stale_target,
                                           stale_parts, 2, ZLINK_SEND_FLAGS_NONE,
                                           NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&stale_parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&stale_parts[1]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (client, ZLINK_OPT_RCVTIMEO,
                                             &wait_ms, sizeof wait_ms));
    zlink_submit_result_t delayed_send = ZLINK_SUBMIT_INVALID_ARGUMENT;
    std::thread selected_sender ([&] {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        zlink_msg_t selected_part;
        if (zlink_msg_init_size (&selected_part, 5) != ZLINK_CONFIG_OK)
            return;
        memcpy (zlink_msg_data (&selected_part), "fresh", 5);
        const zlink_routing_id_t target = rid ("C");
        delayed_send = zlink_send_rid (new_server, &target, &selected_part, 1,
                                       ZLINK_SEND_FLAGS_NONE, NULL, NULL);
        zlink_msg_close (&selected_part);
    });
    recv_data (client, "S", "fresh");
    selected_sender.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, delayed_send);
    TEST_ASSERT_EQUAL_UINT64 (second,
                              zlink_router_recv_route_generation (client));
    no_data (client);

    test_context_socket_close_zero_linger (new_server);
    wait_route (client, "S", second);
    const uint64_t promoted = generation (client, "S");
    TEST_ASSERT_NOT_EQUAL (first, promoted);
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLIN));
    no_data (client);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (old_server);
}

void test_route_change_after_snapshot_keeps_pollroute_ready ()
{
    void *first_server = router ("S");
    void *second_server = router ("S");
    void *client = router ("C");
    char first_endpoint[MAX_SOCKET_STRING];
    char second_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (first_server, first_endpoint, sizeof first_endpoint);
    bind_loopback_ipv4 (second_server, second_endpoint, sizeof second_endpoint);
    connect_as (client, "S", first_endpoint);
    wait_route (client, "S");

    const uint64_t first_generation = generation (client, "S");
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLROUTE));
    connect_as (client, "S", second_endpoint);

    zlink_pollitem_t route_event = {client, 0, ZLINK_POLLROUTE, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&route_event, 1, wait_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_TRUE ((route_event.revents & ZLINK_POLLROUTE) != 0);
    TEST_ASSERT_TRUE ((poll_now (client, ZLINK_POLLROUTE) & ZLINK_POLLROUTE) != 0);

    const uint64_t second_generation = generation (client, "S");
    TEST_ASSERT_NOT_EQUAL (first_generation, second_generation);
    TEST_ASSERT_EQUAL_INT (0, poll_now (client, ZLINK_POLLROUTE));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (second_server);
    test_context_socket_close_zero_linger (first_server);
}

void *dealer_as (const char *rid_, const char *endpoint_)
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, rid_, strlen (rid_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (dealer, ZLINK_OPT_RECONNECT_IVL, &zero,
                                             sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint_));
    return dealer;
}

void dealer_send (void *dealer_, const char *payload_)
{
    zlink_msg_t part;
    const size_t size = strlen (payload_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size));
    memcpy (zlink_msg_data (&part), payload_, size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send (dealer_, &part, 1, ZLINK_SEND_FLAGS_NONE,
                                       NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void wait_route_loss (void *socket_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    do {
        zlink_router_route_t rows[2] = {};
        if (snapshot (socket_, rows, 2) == 0)
            return;
        zlink_pollitem_t item = {socket_, 0, ZLINK_POLLROUTE, 0};
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
    } while (clock_type::now () < deadline);
    TEST_FAIL_MESSAGE ("route loss was not published");
}

// A selected pipe that ends without a successor is not a selection change:
// the records its peer submitted before close stay receivable.
void test_ended_route_without_successor_keeps_pending_records ()
{
    void *server = router ("S");
    const char *endpoint = "inproc://selected-route-ended-keeps";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &wait_ms,
                                             sizeof wait_ms));
    void *dealer = dealer_as ("D", endpoint);
    dealer_send (dealer, "hello");
    recv_data (server, "D", "hello");
    const uint64_t selected = zlink_router_recv_route_generation (server);
    TEST_ASSERT_NOT_EQUAL (0, selected);

    dealer_send (dealer, "p0");
    dealer_send (dealer, "p1");
    dealer_send (dealer, "p2");
    test_context_socket_close (dealer);
    wait_route_loss (server);

    recv_data (server, "D", "p0");
    TEST_ASSERT_EQUAL_UINT64 (selected,
                              zlink_router_recv_route_generation (server));
    recv_data (server, "D", "p1");
    recv_data (server, "D", "p2");
    no_data (server);
    test_context_socket_close_zero_linger (server);
}

// Once another pipe is selected for the RID, the ended pipe's records go.
void test_ended_route_records_discarded_on_next_selection ()
{
    void *server = router ("S");
    const char *endpoint = "inproc://selected-route-ended-replaced";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &wait_ms,
                                             sizeof wait_ms));
    void *first = dealer_as ("D", endpoint);
    dealer_send (first, "hello");
    recv_data (server, "D", "hello");

    dealer_send (first, "stale0");
    dealer_send (first, "stale1");
    test_context_socket_close (first);
    wait_route_loss (server);

    void *second = dealer_as ("D", endpoint);
    dealer_send (second, "fresh");
    wait_route (server, "D");
    const uint64_t selected = generation (server, "D");
    recv_data (server, "D", "fresh");
    TEST_ASSERT_EQUAL_UINT64 (selected,
                              zlink_router_recv_route_generation (server));
    no_data (server);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (server);
}

void dealer_send_two (void *dealer_, const char *first_, const char *second_)
{
    zlink_msg_t parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&parts[0], strlen (first_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&parts[1], strlen (second_)));
    memcpy (zlink_msg_data (&parts[0]), first_, strlen (first_));
    memcpy (zlink_msg_data (&parts[1]), second_, strlen (second_));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send (dealer_, parts, 2, ZLINK_SEND_FLAGS_NONE,
                                       NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[1]));
}

//  Stages a two-part record in the part helper with a one-slot receive.
void stage_two_part_record (void *server_)
{
    zlink_pollitem_t item = {server_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, &error));
    zlink_msg_t slot;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&slot));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUFFER_TOO_SMALL,
                           zlink_router_recv (server_, &source, &token, &slot, 1,
                                              &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&slot));
}

//  A staged record of a pipe that ended without a successor is still the
//  same record on retry, with the generation it was admitted under.
void test_ended_route_staged_record_retries_with_old_generation ()
{
    void *server = router ("S");
    const char *endpoint = "inproc://selected-route-ended-staged-retry";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &wait_ms,
                                             sizeof wait_ms));
    void *dealer = dealer_as ("D", endpoint);
    dealer_send (dealer, "hello");
    recv_data (server, "D", "hello");
    const uint64_t selected = zlink_router_recv_route_generation (server);

    dealer_send_two (dealer, "ab", "cd");
    stage_two_part_record (server);
    test_context_socket_close (dealer);
    wait_route_loss (server);

    zlink_msg_t parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&parts[1]));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (server, &source, &token, parts, 2,
                                              &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_INT (1, source->size);
    TEST_ASSERT_EQUAL_MEMORY ("D", source->data, 1);
    TEST_ASSERT_EQUAL_UINT64 (2, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("ab", zlink_msg_data (&parts[0]), 2);
    TEST_ASSERT_EQUAL_MEMORY ("cd", zlink_msg_data (&parts[1]), 2);
    TEST_ASSERT_EQUAL_UINT64 (selected,
                              zlink_router_recv_route_generation (server));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[1]));
    no_data (server);
    test_context_socket_close_zero_linger (server);
}

//  Once a new pipe is selected for the RID, the staged record of the ended
//  pipe is gone: the retry finds no data and only new records arrive.
void test_ended_route_staged_record_dropped_on_next_selection ()
{
    void *server = router ("S");
    const char *endpoint = "inproc://selected-route-ended-staged-dropped";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &wait_ms,
                                             sizeof wait_ms));
    void *first = dealer_as ("D", endpoint);
    dealer_send (first, "hello");
    recv_data (server, "D", "hello");
    const uint64_t old_generation = zlink_router_recv_route_generation (server);

    dealer_send_two (first, "ab", "cd");
    stage_two_part_record (server);
    test_context_socket_close (first);
    wait_route_loss (server);

    void *second = dealer_as ("D", endpoint);
    wait_route (server, "D");
    const uint64_t new_generation = generation (server, "D");
    TEST_ASSERT_NOT_EQUAL (old_generation, new_generation);

    zlink_msg_t parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&parts[1]));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_router_recv (server, &source, &token, parts, 2,
                                              &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts[1]));

    dealer_send (second, "fresh");
    recv_data (server, "D", "fresh");
    TEST_ASSERT_EQUAL_UINT64 (new_generation,
                              zlink_router_recv_route_generation (server));
    no_data (server);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (server);
}

void test_reciprocal_standby_promotion_loss_and_request_completion ()
{
    void *a = router ("A");
    void *z = router ("Z");
    const char *endpoint_a = "inproc://selected-route-contract-a";
    const char *endpoint_z = "inproc://selected-route-contract-z";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (a, endpoint_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (z, endpoint_z));

    // The first connection is the only selected route before reciprocal
    // arbitration. Its pending request must complete on standby demotion.
    connect_as (z, "A", endpoint_a);
    wait_route (z, "A");
    wait_route (a, "Z");
    const uint64_t z_first = generation (z, "A");
    const uint64_t a_first = generation (a, "Z");

    zlink_msg_t request;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&request, 1));
    *static_cast<char *> (zlink_msg_data (&request)) = 'Q';
    const zlink_routing_id_t target = rid ("A");
    zlink_completion_id_t id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_request (z, &target, &request, 1,
                                          ZLINK_SEND_FLAGS_DONTWAIT, 2000, NULL, &id));
    TEST_ASSERT_NOT_EQUAL (0, id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));
    zlink_pollitem_t input = {a, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&input, 1, wait_ms, &error));
    TEST_ASSERT_TRUE ((input.revents & ZLINK_POLLIN) != 0);

    // Hold the REQUEST in the part helper before its selected pipe is demoted.
    const zlink_routing_id_t *staged_source = NULL;
    zlink_reply_token_t staged_token = 0;
    size_t staged_parts = 0;
    zlink_msg_t staged_slot;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUFFER_TOO_SMALL,
                           zlink_router_recv (a, &staged_source, &staged_token,
                                              &staged_slot, 0, &staged_parts,
                                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, staged_parts);

    // A sorts before Z, so A -> Z replaces Z -> A on both sockets.
    connect_as (a, "Z", endpoint_z);
    wait_route (a, "Z", a_first);
    wait_route (z, "A", z_first);
    const uint64_t a_second = generation (a, "Z");
    const uint64_t z_second = generation (z, "A");
    TEST_ASSERT_NOT_EQUAL (a_first, a_second);
    TEST_ASSERT_NOT_EQUAL (z_first, z_second);
    TEST_ASSERT_EQUAL_INT (0, poll_now (a, ZLINK_POLLIN));
    no_data (a);

    void *completion_poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (completion_poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (completion_poller, z, NULL,
                                             ZLINK_POLLCOMPLETION));
    zlink_poller_event_t completion_event = {};
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poller_wait (completion_poller, &completion_event,
                                              1, wait_ms, &error));
    TEST_ASSERT_TRUE ((completion_event.events & ZLINK_POLLCOMPLETION) != 0);
    zlink_completion_t completion = {};
    completion.struct_size = sizeof completion;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_completion_recv (z, &completion,
                                                  ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED,
                           completion.request_result);
    zlink_completion_close (&completion);
    completion = zlink_completion_t ();
    completion.struct_size = sizeof completion;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_completion_recv (z, &completion,
                                                  ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&completion_poller));

    // Removing only the selected direction promotes the retained standby.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (a, endpoint_z));
    wait_route (a, "Z", a_second);
    wait_route (z, "A", z_second);
    const uint64_t a_third = generation (a, "Z");
    const uint64_t z_third = generation (z, "A");
    TEST_ASSERT_NOT_EQUAL (a_first, a_third);
    TEST_ASSERT_NOT_EQUAL (z_first, z_third);
    send_data (z, "A", "promoted");
    recv_data (a, "Z", "promoted");
    TEST_ASSERT_EQUAL_UINT64 (a_third, zlink_router_recv_route_generation (a));
    no_data (a);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (z, endpoint_a));
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    bool lost = false;
    do {
        zlink_router_route_t rows[2] = {};
        lost = snapshot (a, rows, 2) == 0;
        if (lost)
            break;
        zlink_pollitem_t item = {a, 0, ZLINK_POLLROUTE, 0};
        TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
    } while (clock_type::now () < deadline);
    TEST_ASSERT_TRUE_MESSAGE (lost, "route loss was not published");
    TEST_ASSERT_EQUAL_INT (0, poll_now (a, ZLINK_POLLROUTE));

    test_context_socket_close_zero_linger (z);
    test_context_socket_close_zero_linger (a);
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_snapshot_polling_and_recv_generation);
    RUN_TEST (test_single_data_zero_capacity_retries_same_record);
    RUN_TEST (test_handover_discards_old_records_and_changes_generation);
    RUN_TEST (test_route_change_after_snapshot_keeps_pollroute_ready);
    RUN_TEST (test_reciprocal_standby_promotion_loss_and_request_completion);
    RUN_TEST (test_ended_route_without_successor_keeps_pending_records);
    RUN_TEST (test_ended_route_records_discarded_on_next_selection);
    RUN_TEST (test_ended_route_staged_record_retries_with_old_generation);
    RUN_TEST (test_ended_route_staged_record_dropped_on_next_selection);
    return UNITY_END ();
}
