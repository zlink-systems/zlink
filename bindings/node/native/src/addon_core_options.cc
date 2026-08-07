/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_core_options.h"

int set_socket_option (void *sock, int32_t opt, const void *data, size_t len)
{
    switch (opt) {
        case ZLINK_ROUTER_OPT_MANDATORY:
            return zlink_set_router_option (sock, ZLINK_ROUTER_OPT_MANDATORY, data, len);
        case ZLINK_ROUTER_OPT_PROBE:
            return zlink_set_router_option (sock, ZLINK_ROUTER_OPT_PROBE, data, len);
        case ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID:
            return zlink_set_router_option (sock, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, data, len);
        case ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS:
            return zlink_set_router_option (sock, ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, data, len);
        case ZLINK_ROUTER_OPT_WEIGHT:
            return zlink_set_router_option (sock, ZLINK_ROUTER_OPT_WEIGHT, data, len);
        case ZLINK_DEALER_OPT_PROBE:
            return zlink_set_dealer_option (sock, ZLINK_DEALER_OPT_PROBE, data, len);
        case ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS:
            return zlink_set_dealer_option (sock, ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, data, len);
        case ZLINK_DEALER_OPT_WEIGHT:
            return zlink_set_dealer_option (sock, ZLINK_DEALER_OPT_WEIGHT, data, len);
        case ZLINK_STREAM_OPT_NOTIFY:
            return zlink_set_stream_option (sock, ZLINK_STREAM_OPT_NOTIFY, data, len);
        case ZLINK_PUB_OPT_VERBOSE:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_VERBOSE, data, len);
        case ZLINK_PUB_OPT_VERBOSER:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_VERBOSER, data, len);
        case ZLINK_PUB_OPT_MANUAL:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_MANUAL, data, len);
        case ZLINK_PUB_OPT_MANUAL_LAST_VALUE:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_MANUAL_LAST_VALUE, data, len);
        case ZLINK_PUB_OPT_NODROP:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_NODROP, data, len);
        case ZLINK_PUB_OPT_WELCOME_MSG:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_WELCOME_MSG, data, len);
        case ZLINK_PUB_OPT_TOPICS_COUNT:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_TOPICS_COUNT, data, len);
        case ZLINK_PUB_OPT_APPROVE_SUBSCRIBE:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_APPROVE_SUBSCRIBE, data, len);
        case ZLINK_PUB_OPT_REJECT_SUBSCRIBE:
            return zlink_set_pub_option (sock, ZLINK_PUB_OPT_REJECT_SUBSCRIBE, data, len);
        case ZLINK_SUB_OPT_TOPICS_COUNT:
            return zlink_set_sub_option (sock, ZLINK_SUB_OPT_TOPICS_COUNT, data, len);
        default:
            return zlink_set_option (sock, static_cast<zlink_option_t> (opt), data, len);
    }
}

int get_socket_option (void *sock, int32_t opt, void *data, size_t *len)
{
    switch (opt) {
        case ZLINK_ROUTER_OPT_MANDATORY:
            return zlink_get_router_option (sock, ZLINK_ROUTER_OPT_MANDATORY, data, len);
        case ZLINK_ROUTER_OPT_PROBE:
            return zlink_get_router_option (sock, ZLINK_ROUTER_OPT_PROBE, data, len);
        case ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID:
            return zlink_get_router_option (sock, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, data, len);
        case ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS:
            return zlink_get_router_option (sock, ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, data, len);
        case ZLINK_ROUTER_OPT_WEIGHT:
            return zlink_get_router_option (sock, ZLINK_ROUTER_OPT_WEIGHT, data, len);
        case ZLINK_DEALER_OPT_PROBE:
            return zlink_get_dealer_option (sock, ZLINK_DEALER_OPT_PROBE, data, len);
        case ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS:
            return zlink_get_dealer_option (sock, ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, data, len);
        case ZLINK_DEALER_OPT_WEIGHT:
            return zlink_get_dealer_option (sock, ZLINK_DEALER_OPT_WEIGHT, data, len);
        case ZLINK_STREAM_OPT_NOTIFY:
            return zlink_get_stream_option (sock, ZLINK_STREAM_OPT_NOTIFY, data, len);
        case ZLINK_PUB_OPT_VERBOSE:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_VERBOSE, data, len);
        case ZLINK_PUB_OPT_VERBOSER:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_VERBOSER, data, len);
        case ZLINK_PUB_OPT_MANUAL:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_MANUAL, data, len);
        case ZLINK_PUB_OPT_MANUAL_LAST_VALUE:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_MANUAL_LAST_VALUE, data, len);
        case ZLINK_PUB_OPT_NODROP:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_NODROP, data, len);
        case ZLINK_PUB_OPT_WELCOME_MSG:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_WELCOME_MSG, data, len);
        case ZLINK_PUB_OPT_TOPICS_COUNT:
            return zlink_get_pub_option (sock, ZLINK_PUB_OPT_TOPICS_COUNT, data, len);
        case ZLINK_SUB_OPT_TOPICS_COUNT:
            return zlink_get_sub_option (sock, ZLINK_SUB_OPT_TOPICS_COUNT, data, len);
        default:
            return zlink_get_option (sock, static_cast<zlink_option_t> (opt), data, len);
    }
}

size_t initial_getopt_buffer_len (int32_t opt)
{
    switch (opt) {
        case ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID:
            return 255;
        case ZLINK_OPT_LAST_ENDPOINT:
        case ZLINK_OPT_TLS_CERT:
        case ZLINK_OPT_TLS_KEY:
        case ZLINK_OPT_TLS_CA:
        case ZLINK_OPT_TLS_HOSTNAME:
        case ZLINK_OPT_TLS_PASSWORD:
        case ZLINK_OPT_BINDTODEVICE:
        case ZLINK_OPT_ZMP_METADATA:
            return 256;
        case ZLINK_OPT_MAXMSGSIZE:
            return sizeof (int64_t);
        case ZLINK_OPT_SNDHWM:
        case ZLINK_OPT_RCVHWM:
        case ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES:
            return sizeof (uint64_t);
        default:
            return sizeof (int);
    }
}
