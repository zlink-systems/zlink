/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "core/internal_defs.hpp"
#include "core/options_owner.hpp"

#include <string.h>
#include <unity.h>

namespace
{
bool allocate_loopback_tcp_endpoint (char *endpoint_out_, size_t endpoint_size_)
{
    if (!endpoint_out_ || endpoint_size_ == 0) {
        errno = EINVAL;
        return false;
    }

    for (int attempt = 0; attempt < 256; ++attempt) {
        fd_t fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == retired_fd)
            continue;

        int reuse = 1;
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, as_setsockopt_opt_t (&reuse), sizeof (reuse));

        struct sockaddr_in addr;
        memset (&addr, 0, sizeof (addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind (fd, reinterpret_cast<struct sockaddr *> (&addr), sizeof (addr)) == 0) {
#if defined ZLINK_HAVE_WINDOWS
            int addr_len = sizeof (addr);
#else
            socklen_t addr_len = sizeof (addr);
#endif
            if (getsockname (fd, reinterpret_cast<struct sockaddr *> (&addr), &addr_len) == 0) {
                close (fd);
                snprintf (endpoint_out_, endpoint_size_, "tcp://127.0.0.1:%u",
                          static_cast<unsigned> (ntohs (addr.sin_port)));
                return true;
            }
        }

        close (fd);
    }

    errno = EADDRINUSE;
    return false;
}

} // namespace

void setUp ()
{
}

void tearDown ()
{
}

static void *new_ctx ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    return ctx;
}

static void close_ctx (void *ctx_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_));
}

static void close_monitor_if_open (void **monitor_p_)
{
    if (monitor_p_ && *monitor_p_)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (monitor_p_));
}

void test_typed_raw_socket_options ()
{
    void *ctx = new_ctx ();
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *stream = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
    void *xpub = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    void *xsub = zlink_socket (ctx, ZLINK_SOCKET_XSUB);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (stream);
    TEST_ASSERT_NOT_NULL (xpub);
    TEST_ASSERT_NOT_NULL (xsub);

    uint64_t hwm_value = 42;
    size_t hwm_size = sizeof (hwm_value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm_value, sizeof (hwm_value)));
    hwm_value = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_SNDHWM, &hwm_value, &hwm_size));
    TEST_ASSERT_EQUAL_UINT64 (42, hwm_value);

    const int legacy_hwm = 42;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &legacy_hwm, sizeof (legacy_hwm)));

    //  The byte options take an exact uint64_t on both set and get. A larger
    //  scratch buffer is rejected as well, so a caller can never read a
    //  partially filled value or a value at the wrong width.
    unsigned char oversized_buffer[16] = {0};
    size_t oversized_size = sizeof (oversized_buffer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_get_option (router, ZLINK_OPT_SNDHWM, oversized_buffer, &oversized_size));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    size_t legacy_size = sizeof (legacy_hwm);
    int legacy_out = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_get_option (router, ZLINK_OPT_SNDHWM, &legacy_out, &legacy_size));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    int value = 1;
    size_t size = sizeof (value);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (stream, ZLINK_OPT_IPV6, &value, sizeof (value)));
    value = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (stream, ZLINK_OPT_IPV6, &value, &size));
    TEST_ASSERT_EQUAL_INT (1, value);

    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (xpub, ZLINK_OPT_TYPE, &value, &size));
    TEST_ASSERT_EQUAL_INT (ZLINK_CORE_SOCKET_XPUB, value);

    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &value, &size));
    TEST_ASSERT_EQUAL_INT (ZLINK_RID_DUPLICATE_REJECT, value);

    value = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &value, sizeof (value)));
    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &value, &size));
    TEST_ASSERT_EQUAL_INT (ZLINK_RID_DUPLICATE_HANDOVER, value);

    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_SUBMIT_RETRY_MODE, &value, &size));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_RETRY_OFF, value);

    value = ZLINK_SUBMIT_RETRY_LOCAL_FAILURE;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SUBMIT_RETRY_MODE, &value, sizeof (value)));
    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_SUBMIT_RETRY_MODE, &value, &size));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_RETRY_LOCAL_FAILURE, value);

    value = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, &value, sizeof (value)));
    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, &value, &size));
    TEST_ASSERT_EQUAL_INT (100, value);

    value = 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, &value, sizeof (value)));
    value = -1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (router, ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, &value, &size));
    TEST_ASSERT_EQUAL_INT (2, value);

    value = 17;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_set_option (router, ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, &value, sizeof (value)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    value = 1;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &value, sizeof (value)));
    value = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &value, &size));
    TEST_ASSERT_EQUAL_INT (1, value);

    value = 4321;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (router, ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS,
                                                        &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_router_option (router, ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, &value, &size));
    TEST_ASSERT_EQUAL_INT (4321, value);

    value = 50;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &value, &size));
    TEST_ASSERT_EQUAL_INT (50, value);

    value = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_PROBE, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_dealer_option (dealer, ZLINK_DEALER_OPT_PROBE, &value, &size));
    TEST_ASSERT_EQUAL_INT (1, value);

    value = 3210;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
                                                        &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_dealer_option (dealer, ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, &value, &size));
    TEST_ASSERT_EQUAL_INT (3210, value);

    value = 25;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, &size));
    TEST_ASSERT_EQUAL_INT (25, value);

    //  The weight range is 0..10000 on both socket types and out-of-range
    //  input is rejected instead of clamped.
    value = 10000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, &size));
    TEST_ASSERT_EQUAL_INT (10000, value);

    value = 10001;
    TEST_ASSERT_NOT_EQUAL (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &value, &size));
    TEST_ASSERT_EQUAL_INT (10000, value);

    value = 10000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &value, sizeof (value)));
    value = 10001;
    TEST_ASSERT_NOT_EQUAL (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &value, &size));
    TEST_ASSERT_EQUAL_INT (10000, value);

    value = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &value, sizeof (value)));
    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &value, &size));
    TEST_ASSERT_EQUAL_INT (1, value);

    TEST_ASSERT_NOT_EQUAL (ZLINK_CONFIG_OK, zlink_set_routing_id (stream, "s1", 2));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    value = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (xpub, ZLINK_PUB_OPT_NODROP, &value, sizeof (value)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub, "topic"));

    value = 0;
    size = sizeof (value);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_sub_option (xsub, ZLINK_SUB_OPT_TOPICS_COUNT, &value, &size));
    TEST_ASSERT_EQUAL_INT (1, value);

    char filter[32];
    size_t filter_len = sizeof (filter);
    int is_pattern = -1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_subscription_at (xsub, 0, filter, &filter_len, &is_pattern));
    TEST_ASSERT_EQUAL_UINT (5, (unsigned int) filter_len);
    TEST_ASSERT_EQUAL_MEMORY ("topic", filter, 5);
    TEST_ASSERT_EQUAL_INT (0, is_pattern);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (router));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (stream));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (xpub));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (xsub));
    close_ctx (ctx);
}

void test_option_owner_map_matches_domains ()
{
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_SNDHWM));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_transport_network,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_TCP_NODELAY));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_transport_network,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_BINDTODEVICE));

#ifdef ZLINK_HAVE_TLS
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_protocol_metadata,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_TLS_CERT));
#endif

    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_TOPICS_COUNT));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_LAST_ENDPOINT));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_unknown,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_BLOCKY));

    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::common_option_owner_of (ZLINK_OPT_SNDHWM));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::common_option_owner_of (ZLINK_OPT_RID_DUPLICATE_POLICY));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::common_option_owner_of (ZLINK_OPT_SUBMIT_RETRY_MODE));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::option_owner_of (ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_transport_network,
                           zlink::common_option_owner_of (ZLINK_OPT_TCP_NODELAY));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::common_option_owner_of (ZLINK_OPT_LAST_ENDPOINT));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_unknown,
                           zlink::common_option_owner_of (ZLINK_OPT_BLOCKY));

    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::router_option_owner_of (ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::router_option_owner_of (ZLINK_ROUTER_OPT_WEIGHT));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::dealer_option_owner_of (ZLINK_DEALER_OPT_PROBE));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::dealer_option_owner_of (ZLINK_DEALER_OPT_WEIGHT));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_core_socket,
                           zlink::stream_option_owner_of (ZLINK_STREAM_OPT_NOTIFY));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::pub_option_owner_of (ZLINK_PUB_OPT_MANUAL_LAST_VALUE));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_socket_specific,
                           zlink::sub_option_owner_of (ZLINK_SUB_OPT_TOPICS_COUNT));

    TEST_ASSERT_EQUAL_INT (
      zlink::options_owner_core_socket,
      zlink::option_owner_of_bag_field (zlink::options_bag_field_monitor_event_version));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_protocol_metadata,
                           zlink::option_owner_of_bag_field (zlink::options_bag_field_hello_msg));
    TEST_ASSERT_EQUAL_INT (
      zlink::options_owner_protocol_metadata,
      zlink::option_owner_of_bag_field (zlink::options_bag_field_disconnect_msg));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_protocol_metadata,
                           zlink::option_owner_of_bag_field (zlink::options_bag_field_hiccup_msg));
    TEST_ASSERT_EQUAL_INT (zlink::options_owner_transport_network,
                           zlink::option_owner_of_bag_field (zlink::options_bag_field_busy_poll));

    TEST_ASSERT_EQUAL_STRING (
      "core-socket", zlink::option_owner_name (zlink::option_owner_of (ZLINK_INTERNAL_OPT_SNDHWM)));
    TEST_ASSERT_EQUAL_STRING (
      "transport-network",
      zlink::option_owner_name (zlink::option_owner_of (ZLINK_INTERNAL_OPT_TCP_NODELAY)));
    TEST_ASSERT_EQUAL_STRING ("socket-specific", zlink::option_owner_name (zlink::option_owner_of (
                                                    ZLINK_INTERNAL_OPT_TOPICS_COUNT)));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_typed_raw_socket_options);
    RUN_TEST (test_option_owner_map_matches_domains);
    return UNITY_END ();
}
