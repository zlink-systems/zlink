/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/zlink_option_internal.hpp"

#include <stddef.h>

namespace
{
template <size_t N>
const option_descriptor_t *lookup_option_descriptor (const option_descriptor_t (&table_)[N],
                                                     int option_)
{
    for (size_t i = 0; i < N; ++i) {
        if (table_[i].public_option == option_)
            return &table_[i];
    }

    errno = EINVAL;
    return NULL;
}

const option_descriptor_t common_option_table[] = {
  {ZLINK_OPT_AFFINITY, ZLINK_INTERNAL_OPT_AFFINITY, false},
  {ZLINK_OPT_RATE, ZLINK_INTERNAL_OPT_RATE, false},
  {ZLINK_OPT_RECOVERY_IVL, ZLINK_INTERNAL_OPT_RECOVERY_IVL, false},
  {ZLINK_OPT_SNDBUF, ZLINK_INTERNAL_OPT_SNDBUF, false},
  {ZLINK_OPT_RCVBUF, ZLINK_INTERNAL_OPT_RCVBUF, false},
  {ZLINK_OPT_FD, ZLINK_INTERNAL_OPT_FD, false},
  {ZLINK_OPT_EVENTS, ZLINK_INTERNAL_OPT_EVENTS, false},
  {ZLINK_OPT_TYPE, ZLINK_INTERNAL_OPT_TYPE, false},
  {ZLINK_OPT_LINGER, ZLINK_INTERNAL_OPT_LINGER, false},
  {ZLINK_OPT_RECONNECT_IVL, ZLINK_INTERNAL_OPT_RECONNECT_IVL, false},
  {ZLINK_OPT_BACKLOG, ZLINK_INTERNAL_OPT_BACKLOG, false},
  {ZLINK_OPT_RECONNECT_IVL_MAX, ZLINK_INTERNAL_OPT_RECONNECT_IVL_MAX, false},
  {ZLINK_OPT_MAXMSGSIZE, ZLINK_INTERNAL_OPT_MAXMSGSIZE, false},
  {ZLINK_OPT_SNDHWM, ZLINK_INTERNAL_OPT_SNDHWM, false},
  {ZLINK_OPT_RCVHWM, ZLINK_INTERNAL_OPT_RCVHWM, false},
  {ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES, ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES, false},
  {ZLINK_OPT_MULTICAST_HOPS, ZLINK_INTERNAL_OPT_MULTICAST_HOPS, false},
  {ZLINK_OPT_RCVTIMEO, ZLINK_INTERNAL_OPT_RCVTIMEO, false},
  {ZLINK_OPT_SNDTIMEO, ZLINK_INTERNAL_OPT_SNDTIMEO, false},
  {ZLINK_OPT_LAST_ENDPOINT, ZLINK_INTERNAL_OPT_LAST_ENDPOINT, false},
  {ZLINK_OPT_TCP_KEEPALIVE, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE, false},
  {ZLINK_OPT_TCP_KEEPALIVE_CNT, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_CNT, false},
  {ZLINK_OPT_TCP_KEEPALIVE_IDLE, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_IDLE, false},
  {ZLINK_OPT_TCP_KEEPALIVE_INTVL, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_INTVL, false},
  {ZLINK_OPT_IMMEDIATE, ZLINK_INTERNAL_OPT_IMMEDIATE, false},
  {ZLINK_OPT_IPV6, ZLINK_INTERNAL_OPT_IPV6, false},
  {ZLINK_OPT_CONFLATE, ZLINK_INTERNAL_OPT_CONFLATE, false},
  {ZLINK_OPT_TOS, ZLINK_INTERNAL_OPT_TOS, false},
  {ZLINK_OPT_HANDSHAKE_IVL, ZLINK_INTERNAL_OPT_HANDSHAKE_IVL, false},
  {ZLINK_OPT_BLOCKY, ZLINK_INTERNAL_OPT_BLOCKY, false},
  {ZLINK_OPT_INVERT_MATCHING, ZLINK_INTERNAL_OPT_INVERT_MATCHING, false},
  {ZLINK_OPT_SUBMIT_RETRY_MODE, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_MODE, false},
  {ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_TIMEOUT, false},
  {ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS, false},
  {ZLINK_OPT_CONNECT_TIMEOUT, ZLINK_INTERNAL_OPT_CONNECT_TIMEOUT, false},
  {ZLINK_OPT_TCP_MAXRT, ZLINK_INTERNAL_OPT_TCP_MAXRT, false},
  {ZLINK_OPT_MULTICAST_MAXTPDU, ZLINK_INTERNAL_OPT_MULTICAST_MAXTPDU, false},
  {ZLINK_OPT_BINDTODEVICE, ZLINK_INTERNAL_OPT_BINDTODEVICE, false},
  {ZLINK_OPT_TLS_CERT, ZLINK_INTERNAL_OPT_TLS_CERT, false},
  {ZLINK_OPT_TLS_KEY, ZLINK_INTERNAL_OPT_TLS_KEY, false},
  {ZLINK_OPT_TLS_CA, ZLINK_INTERNAL_OPT_TLS_CA, false},
  {ZLINK_OPT_TLS_VERIFY, ZLINK_INTERNAL_OPT_TLS_VERIFY, false},
  {ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT, ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT, false},
  {ZLINK_OPT_TLS_HOSTNAME, ZLINK_INTERNAL_OPT_TLS_HOSTNAME, false},
  {ZLINK_OPT_TLS_TRUST_SYSTEM, ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM, false},
  {ZLINK_OPT_TLS_PASSWORD, ZLINK_INTERNAL_OPT_TLS_PASSWORD, false},
  {ZLINK_OPT_ZMP_METADATA, ZLINK_INTERNAL_OPT_ZMP_METADATA, false},
  {ZLINK_OPT_TCP_NODELAY, ZLINK_INTERNAL_OPT_TCP_NODELAY, false},
  {ZLINK_OPT_RID_DUPLICATE_POLICY, ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY, false},
  {ZLINK_OPT_ROUTE_VALUE_MAX_SIZE, ZLINK_OPT_ROUTE_VALUE_MAX_SIZE, true},
};

const option_descriptor_t router_option_table[] = {
  {ZLINK_ROUTER_OPT_MANDATORY, ZLINK_INTERNAL_OPT_ROUTER_MANDATORY, false},
  {ZLINK_ROUTER_OPT_PROBE, ZLINK_INTERNAL_OPT_PROBE_ROUTER, false},
  {ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, ZLINK_INTERNAL_OPT_CONNECT_ROUTING_ID, false},
  {ZLINK_ROUTER_OPT_WEIGHT, ZLINK_INTERNAL_OPT_PEER_WEIGHT, false},
};

const option_descriptor_t dealer_option_table[] = {
  {ZLINK_DEALER_OPT_PROBE, ZLINK_INTERNAL_OPT_PROBE_ROUTER, false},
  {ZLINK_DEALER_OPT_WEIGHT, ZLINK_INTERNAL_OPT_PEER_WEIGHT, false},
};

const option_descriptor_t stream_option_table[] = {
  {ZLINK_STREAM_OPT_NOTIFY, ZLINK_INTERNAL_OPT_STREAM_NOTIFY, false},
};

const option_descriptor_t pub_option_table[] = {
  {ZLINK_PUB_OPT_VERBOSE, ZLINK_INTERNAL_OPT_XPUB_VERBOSE, false},
  {ZLINK_PUB_OPT_VERBOSER, ZLINK_INTERNAL_OPT_XPUB_VERBOSER, false},
  {ZLINK_PUB_OPT_MANUAL, ZLINK_INTERNAL_OPT_XPUB_MANUAL, false},
  {ZLINK_PUB_OPT_MANUAL_LAST_VALUE, ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE, false},
  {ZLINK_PUB_OPT_NODROP, ZLINK_INTERNAL_OPT_XPUB_NODROP, false},
  {ZLINK_PUB_OPT_WELCOME_MSG, ZLINK_INTERNAL_OPT_XPUB_WELCOME_MSG, false},
  {ZLINK_PUB_OPT_TOPICS_COUNT, ZLINK_INTERNAL_OPT_TOPICS_COUNT, false},
  {ZLINK_PUB_OPT_APPROVE_SUBSCRIBE, ZLINK_INTERNAL_OPT_SUBSCRIBE, false},
  {ZLINK_PUB_OPT_REJECT_SUBSCRIBE, ZLINK_INTERNAL_OPT_UNSUBSCRIBE, false},
};

const option_descriptor_t sub_option_table[] = {
  {ZLINK_SUB_OPT_TOPICS_COUNT, ZLINK_INTERNAL_OPT_TOPICS_COUNT, false},
};
}

int map_common_option (zlink_option_t option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (common_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

int map_router_option (zlink_router_option_t option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (router_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

int map_dealer_option (zlink_dealer_option_t option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (dealer_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

int map_stream_option (zlink_stream_option_t option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (stream_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

int map_pub_option (int option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (pub_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

int map_sub_option (int option_)
{
    const option_descriptor_t *descriptor = lookup_option_descriptor (sub_option_table, option_);
    return descriptor ? descriptor->internal_option : -1;
}

const option_descriptor_t *lookup_common_option (zlink_option_t option_)
{
    return lookup_option_descriptor (common_option_table, option_);
}
