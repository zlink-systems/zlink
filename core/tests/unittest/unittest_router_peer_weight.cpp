/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/pipe.hpp"
#include "sockets/dealer/dealer.hpp"

#include <unity.h>
#include <cstring>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

namespace
{
void *create_sync_socket (int type_)
{
    void *socket = zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    return socket;
}

void close_sync_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}


}

int recv_one_weighted_router_index (void *router1_, void *router2_)
{
    void *routers[] = {router1_, router2_};
    for (size_t i = 0; i != 2; ++i) {
        char buffer[32];
        zlink_routing_id_t source = {};
        const int size = test_recv_router (
          routers[i], buffer, sizeof (buffer), ZLINK_DONTWAIT, &source);
        if (size < 0) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            continue;
        }
        TEST_ASSERT_GREATER_THAN_INT (0, source.size);
        TEST_ASSERT_EQUAL_INT (1, size);
        TEST_ASSERT_EQUAL_MEMORY ("x", buffer, 1);
        return static_cast<int> (i);
    }
    return -1;
}

void test_unpaired_inproc_peer_weight_is_not_application_data ()
{
    void *dealer = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *router1 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *router2 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    const int weight1 = 1;
    const int weight2 = 3;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router1, ZLINK_ROUTER_OPT_WEIGHT, &weight1,
                               sizeof (weight1)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router2, ZLINK_ROUTER_OPT_WEIGHT, &weight2,
                               sizeof (weight2)));
    contract_socket_pair_t first_pair (dealer, router1, 0);
    contract_socket_pair_t second_pair (dealer, router2, 0);
    first_pair.pump ();
    second_pair.pump ();
    zlink::dealer_t *const dealer_core = static_cast<zlink::dealer_t *> (
      as_socket_handle (dealer).socket);
    TEST_ASSERT_EQUAL_UINT32 (1, dealer_core->test_peer_weight_count (1));
    TEST_ASSERT_EQUAL_UINT32 (1, dealer_core->test_peer_weight_count (3));
    char raw[32];
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv (dealer, raw, sizeof (raw), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    int counts[2] = {0, 0};
    for (int i = 0; i < 40; ++i) {
        zlink::msg_t message;
        TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
        memcpy (message.data (), "x", 1);
        TEST_ASSERT_SUCCESS_ERRNO (dealer_core->send (&message, 0));
        TEST_ASSERT_SUCCESS_ERRNO (message.close ());
        first_pair.pump ();
        second_pair.pump ();
        const int selected = recv_one_weighted_router_index (router1, router2);
        TEST_ASSERT_TRUE (selected == 0 || selected == 1);
        ++counts[selected];
    }
    TEST_ASSERT_EQUAL_INT (10, counts[0]);
    TEST_ASSERT_EQUAL_INT (30, counts[1]);

    //  Zero removes one peer from selection immediately. The update still
    //  travels only as an owner command, so a raw receive remains empty.
    const int zero = 0;
    const int one = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router1, ZLINK_ROUTER_OPT_WEIGHT, &zero,
                               sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router2, ZLINK_ROUTER_OPT_WEIGHT, &one,
                               sizeof (one)));
    first_pair.pump ();
    second_pair.pump ();
    TEST_ASSERT_EQUAL_UINT32 (1, dealer_core->test_peer_weight_count (0));
    TEST_ASSERT_EQUAL_UINT32 (1, dealer_core->test_peer_weight_count (1));
    for (int i = 0; i < 12; ++i) {
        zlink::msg_t message;
        TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
        memcpy (message.data (), "x", 1);
        TEST_ASSERT_SUCCESS_ERRNO (dealer_core->send (&message, 0));
        TEST_ASSERT_SUCCESS_ERRNO (message.close ());
        first_pair.pump ();
        second_pair.pump ();
        TEST_ASSERT_EQUAL_INT (
          1, recv_one_weighted_router_index (router1, router2));
    }
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv (dealer, raw, sizeof (raw), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    close_sync_socket (router2);
    close_sync_socket (router1);
    close_sync_socket (dealer);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_unpaired_inproc_peer_weight_is_not_application_data);
    return UNITY_END ();
}
