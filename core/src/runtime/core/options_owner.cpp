/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/internal_defs.hpp"
#include "core/options_owner.hpp"

namespace
{
zlink::options_owner_t common_option_owner_lookup (zlink_option_t option_)
{
    switch (option_) {
        case ZLINK_OPT_AFFINITY:
        case ZLINK_OPT_RATE:
        case ZLINK_OPT_RECOVERY_IVL:
        case ZLINK_OPT_TYPE:
        case ZLINK_OPT_MAXMSGSIZE:
        case ZLINK_OPT_SNDHWM:
        case ZLINK_OPT_RCVHWM:
        case ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES:
        case ZLINK_OPT_MULTICAST_HOPS:
        case ZLINK_OPT_IMMEDIATE:
        case ZLINK_OPT_IPV6:
        case ZLINK_OPT_CONFLATE:
        case ZLINK_OPT_HANDSHAKE_IVL:
        case ZLINK_OPT_INVERT_MATCHING:
        case ZLINK_OPT_SUBMIT_RETRY_MODE:
        case ZLINK_OPT_SUBMIT_RETRY_TIMEOUT:
        case ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS:
        case ZLINK_OPT_ZMP_METADATA:
        case ZLINK_OPT_RID_DUPLICATE_POLICY:
            return zlink::options_owner_core_socket;

        case ZLINK_OPT_SNDBUF:
        case ZLINK_OPT_RCVBUF:
        case ZLINK_OPT_LINGER:
        case ZLINK_OPT_RECONNECT_IVL:
        case ZLINK_OPT_BACKLOG:
        case ZLINK_OPT_RECONNECT_IVL_MAX:
        case ZLINK_OPT_RCVTIMEO:
        case ZLINK_OPT_SNDTIMEO:
        case ZLINK_OPT_TCP_KEEPALIVE:
        case ZLINK_OPT_TCP_KEEPALIVE_CNT:
        case ZLINK_OPT_TCP_KEEPALIVE_IDLE:
        case ZLINK_OPT_TCP_KEEPALIVE_INTVL:
        case ZLINK_OPT_TOS:
        case ZLINK_OPT_CONNECT_TIMEOUT:
        case ZLINK_OPT_TCP_MAXRT:
        case ZLINK_OPT_BINDTODEVICE:
        case ZLINK_OPT_TCP_NODELAY:
            return zlink::options_owner_transport_network;

        case ZLINK_OPT_TLS_CERT:
        case ZLINK_OPT_TLS_KEY:
        case ZLINK_OPT_TLS_CA:
        case ZLINK_OPT_TLS_VERIFY:
        case ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT:
        case ZLINK_OPT_TLS_HOSTNAME:
        case ZLINK_OPT_TLS_TRUST_SYSTEM:
        case ZLINK_OPT_TLS_PASSWORD:
            return zlink::options_owner_protocol_metadata;

        case ZLINK_OPT_FD:
        case ZLINK_OPT_EVENTS:
        case ZLINK_OPT_LAST_ENDPOINT:
            return zlink::options_owner_socket_specific;

        case ZLINK_OPT_BLOCKY:
        default:
            return zlink::options_owner_unknown;
    }
}
} // namespace

zlink::options_owner_t zlink::option_owner_of (int option_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_AFFINITY:
        case ZLINK_INTERNAL_OPT_ROUTING_ID:
        case ZLINK_INTERNAL_OPT_RATE:
        case ZLINK_INTERNAL_OPT_RECOVERY_IVL:
        case ZLINK_INTERNAL_OPT_MAXMSGSIZE:
        case ZLINK_INTERNAL_OPT_MULTICAST_HOPS:
        case ZLINK_INTERNAL_OPT_MULTICAST_MAXTPDU:
        case ZLINK_INTERNAL_OPT_SNDHWM:
        case ZLINK_INTERNAL_OPT_RCVHWM:
        case ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES:
        case ZLINK_INTERNAL_OPT_IPV6:
        case ZLINK_INTERNAL_OPT_IMMEDIATE:
        case ZLINK_INTERNAL_OPT_CONFLATE:
        case ZLINK_INTERNAL_OPT_HANDSHAKE_IVL:
        case ZLINK_INTERNAL_OPT_INVERT_MATCHING:
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_MODE:
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_TIMEOUT:
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS:
        case ZLINK_INTERNAL_OPT_STREAM_NOTIFY:
        case ZLINK_INTERNAL_OPT_TYPE:
        case ZLINK_INTERNAL_OPT_ZMP_METADATA:
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
        case ZLINK_INTERNAL_OPT_PEER_WEIGHT:
            return options_owner_core_socket;

        case ZLINK_INTERNAL_OPT_SNDBUF:
        case ZLINK_INTERNAL_OPT_RCVBUF:
        case ZLINK_INTERNAL_OPT_LINGER:
        case ZLINK_INTERNAL_OPT_RECONNECT_IVL:
        case ZLINK_INTERNAL_OPT_BACKLOG:
        case ZLINK_INTERNAL_OPT_RECONNECT_IVL_MAX:
        case ZLINK_INTERNAL_OPT_RCVTIMEO:
        case ZLINK_INTERNAL_OPT_SNDTIMEO:
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE:
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_CNT:
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_IDLE:
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_INTVL:
        case ZLINK_INTERNAL_OPT_TOS:
        case ZLINK_INTERNAL_OPT_CONNECT_TIMEOUT:
        case ZLINK_INTERNAL_OPT_TCP_MAXRT:
        case ZLINK_INTERNAL_OPT_BINDTODEVICE:
        case ZLINK_INTERNAL_OPT_TCP_NODELAY:
            return options_owner_transport_network;

#ifdef ZLINK_HAVE_TLS
        case ZLINK_INTERNAL_OPT_TLS_CERT:
        case ZLINK_INTERNAL_OPT_TLS_KEY:
        case ZLINK_INTERNAL_OPT_TLS_CA:
        case ZLINK_INTERNAL_OPT_TLS_VERIFY:
        case ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT:
        case ZLINK_INTERNAL_OPT_TLS_HOSTNAME:
        case ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM:
        case ZLINK_INTERNAL_OPT_TLS_PASSWORD:
            return options_owner_protocol_metadata;
#endif

        case ZLINK_INTERNAL_OPT_SUBSCRIBE:
        case ZLINK_INTERNAL_OPT_UNSUBSCRIBE:
        case ZLINK_INTERNAL_OPT_ROUTER_MANDATORY:
        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
        case ZLINK_INTERNAL_OPT_CONNECT_ROUTING_ID:
        case ZLINK_INTERNAL_OPT_XPUB_VERBOSE:
        case ZLINK_INTERNAL_OPT_XPUB_NODROP:
        case ZLINK_INTERNAL_OPT_XPUB_MANUAL:
        case ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE:
        case ZLINK_INTERNAL_OPT_XPUB_WELCOME_MSG:
        case ZLINK_INTERNAL_OPT_XPUB_VERBOSER:
        case ZLINK_INTERNAL_OPT_TOPICS_COUNT:
        case ZLINK_INTERNAL_OPT_FD:
        case ZLINK_INTERNAL_OPT_EVENTS:
        case ZLINK_INTERNAL_OPT_LAST_ENDPOINT:
            return options_owner_socket_specific;

        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::option_owner_of_bag_field (options_bag_field_t field_)
{
    switch (field_) {
        case options_bag_field_monitor_event_version:
            return options_owner_core_socket;
        case options_bag_field_hello_msg:
        case options_bag_field_disconnect_msg:
        case options_bag_field_hiccup_msg:
            return options_owner_protocol_metadata;
        case options_bag_field_busy_poll:
            return options_owner_transport_network;
        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::common_option_owner_of (zlink_option_t option_)
{
    return common_option_owner_lookup (option_);
}

zlink::options_owner_t zlink::router_option_owner_of (zlink_router_option_t option_)
{
    switch (option_) {
        case ZLINK_ROUTER_OPT_MANDATORY:
        case ZLINK_ROUTER_OPT_PROBE:
        case ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID:
            return options_owner_socket_specific;
        case ZLINK_ROUTER_OPT_WEIGHT:
            return options_owner_core_socket;
        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::dealer_option_owner_of (zlink_dealer_option_t option_)
{
    switch (option_) {
        case ZLINK_DEALER_OPT_PROBE:
            return options_owner_socket_specific;
        case ZLINK_DEALER_OPT_WEIGHT:
            return options_owner_core_socket;
        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::stream_option_owner_of (zlink_stream_option_t option_)
{
    switch (option_) {
        case ZLINK_STREAM_OPT_NOTIFY:
            return options_owner_core_socket;
        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::pub_option_owner_of (int option_)
{
    switch (option_) {
        case ZLINK_PUB_OPT_VERBOSE:
        case ZLINK_PUB_OPT_VERBOSER:
        case ZLINK_PUB_OPT_MANUAL:
        case ZLINK_PUB_OPT_MANUAL_LAST_VALUE:
        case ZLINK_PUB_OPT_NODROP:
        case ZLINK_PUB_OPT_WELCOME_MSG:
        case ZLINK_PUB_OPT_TOPICS_COUNT:
        case ZLINK_PUB_OPT_APPROVE_SUBSCRIBE:
        case ZLINK_PUB_OPT_REJECT_SUBSCRIBE:
            return options_owner_socket_specific;
        default:
            return options_owner_unknown;
    }
}

zlink::options_owner_t zlink::sub_option_owner_of (int option_)
{
    switch (option_) {
        case ZLINK_SUB_OPT_TOPICS_COUNT:
            return options_owner_socket_specific;
        default:
            return options_owner_unknown;
    }
}

const char *zlink::option_owner_name (options_owner_t owner_)
{
    switch (owner_) {
        case options_owner_core_socket:
            return "core-socket";
        case options_owner_transport_network:
            return "transport-network";
        case options_owner_protocol_metadata:
            return "protocol-metadata";
        case options_owner_socket_specific:
            return "socket-specific";
        default:
            return "unknown";
    }
}
