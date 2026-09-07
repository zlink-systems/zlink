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
  {ZLINK_OPT_AFFINITY, ZLINK_INTERNAL_OPT_AFFINITY},
  {ZLINK_OPT_RATE, ZLINK_INTERNAL_OPT_RATE},
  {ZLINK_OPT_RECOVERY_IVL, ZLINK_INTERNAL_OPT_RECOVERY_IVL},
  {ZLINK_OPT_SNDBUF, ZLINK_INTERNAL_OPT_SNDBUF},
  {ZLINK_OPT_RCVBUF, ZLINK_INTERNAL_OPT_RCVBUF},
  {ZLINK_OPT_FD, ZLINK_INTERNAL_OPT_FD},
  {ZLINK_OPT_EVENTS, ZLINK_INTERNAL_OPT_EVENTS},
  {ZLINK_OPT_TYPE, ZLINK_INTERNAL_OPT_TYPE},
  {ZLINK_OPT_LINGER, ZLINK_INTERNAL_OPT_LINGER},
  {ZLINK_OPT_RECONNECT_IVL, ZLINK_INTERNAL_OPT_RECONNECT_IVL},
  {ZLINK_OPT_BACKLOG, ZLINK_INTERNAL_OPT_BACKLOG},
  {ZLINK_OPT_RECONNECT_IVL_MAX, ZLINK_INTERNAL_OPT_RECONNECT_IVL_MAX},
  {ZLINK_OPT_MAXMSGSIZE, ZLINK_INTERNAL_OPT_MAXMSGSIZE},
  {ZLINK_OPT_SNDHWM, ZLINK_INTERNAL_OPT_SNDHWM},
  {ZLINK_OPT_RCVHWM, ZLINK_INTERNAL_OPT_RCVHWM},
  {ZLINK_OPT_MULTICAST_HOPS, ZLINK_INTERNAL_OPT_MULTICAST_HOPS},
  {ZLINK_OPT_RCVTIMEO, ZLINK_INTERNAL_OPT_RCVTIMEO},
  {ZLINK_OPT_SNDTIMEO, ZLINK_INTERNAL_OPT_SNDTIMEO},
  {ZLINK_OPT_LAST_ENDPOINT, ZLINK_INTERNAL_OPT_LAST_ENDPOINT},
  {ZLINK_OPT_TCP_KEEPALIVE, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE},
  {ZLINK_OPT_TCP_KEEPALIVE_CNT, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_CNT},
  {ZLINK_OPT_TCP_KEEPALIVE_IDLE, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_IDLE},
  {ZLINK_OPT_TCP_KEEPALIVE_INTVL, ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_INTVL},
  {ZLINK_OPT_IMMEDIATE, ZLINK_INTERNAL_OPT_IMMEDIATE},
  {ZLINK_OPT_IPV6, ZLINK_INTERNAL_OPT_IPV6},
  {ZLINK_OPT_CONFLATE, ZLINK_INTERNAL_OPT_CONFLATE},
  {ZLINK_OPT_TOS, ZLINK_INTERNAL_OPT_TOS},
  {ZLINK_OPT_HANDSHAKE_IVL, ZLINK_INTERNAL_OPT_HANDSHAKE_IVL},
  {ZLINK_OPT_BLOCKY, ZLINK_INTERNAL_OPT_BLOCKY},
  {ZLINK_OPT_INVERT_MATCHING, ZLINK_INTERNAL_OPT_INVERT_MATCHING},
  {ZLINK_OPT_SUBMIT_RETRY_MODE, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_MODE},
  {ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_TIMEOUT},
  {ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS},
  {ZLINK_OPT_PENDING_MAX_MSGS, ZLINK_INTERNAL_OPT_PENDING_MAX_MSGS},
  {ZLINK_OPT_PENDING_MAX_BYTES, ZLINK_INTERNAL_OPT_PENDING_MAX_BYTES},
  {ZLINK_OPT_CONNECT_TIMEOUT, ZLINK_INTERNAL_OPT_CONNECT_TIMEOUT},
  {ZLINK_OPT_TCP_MAXRT, ZLINK_INTERNAL_OPT_TCP_MAXRT},
  {ZLINK_OPT_MULTICAST_MAXTPDU, ZLINK_INTERNAL_OPT_MULTICAST_MAXTPDU},
  {ZLINK_OPT_BINDTODEVICE, ZLINK_INTERNAL_OPT_BINDTODEVICE},
  {ZLINK_OPT_TLS_CERT, ZLINK_INTERNAL_OPT_TLS_CERT},
  {ZLINK_OPT_TLS_KEY, ZLINK_INTERNAL_OPT_TLS_KEY},
  {ZLINK_OPT_TLS_CA, ZLINK_INTERNAL_OPT_TLS_CA},
  {ZLINK_OPT_TLS_VERIFY, ZLINK_INTERNAL_OPT_TLS_VERIFY},
  {ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT, ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT},
  {ZLINK_OPT_TLS_HOSTNAME, ZLINK_INTERNAL_OPT_TLS_HOSTNAME},
  {ZLINK_OPT_TLS_TRUST_SYSTEM, ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM},
  {ZLINK_OPT_TLS_PASSWORD, ZLINK_INTERNAL_OPT_TLS_PASSWORD},
  {ZLINK_OPT_ZMP_METADATA, ZLINK_INTERNAL_OPT_ZMP_METADATA},
  {ZLINK_OPT_TCP_NODELAY, ZLINK_INTERNAL_OPT_TCP_NODELAY},
  {ZLINK_OPT_RID_DUPLICATE_POLICY, ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY},
};

const option_descriptor_t router_option_table[] = {
  {ZLINK_ROUTER_OPT_MANDATORY, ZLINK_INTERNAL_OPT_ROUTER_MANDATORY},
  {ZLINK_ROUTER_OPT_PROBE, ZLINK_INTERNAL_OPT_PROBE_ROUTER},
  {ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, ZLINK_INTERNAL_OPT_CONNECT_ROUTING_ID},
  {ZLINK_ROUTER_OPT_WEIGHT, ZLINK_INTERNAL_OPT_PEER_WEIGHT},
};

const option_descriptor_t dealer_option_table[] = {
  {ZLINK_DEALER_OPT_PROBE, ZLINK_INTERNAL_OPT_PROBE_ROUTER},
  {ZLINK_DEALER_OPT_WEIGHT, ZLINK_INTERNAL_OPT_PEER_WEIGHT},
};

const option_descriptor_t stream_option_table[] = {
  {ZLINK_STREAM_OPT_NOTIFY, ZLINK_INTERNAL_OPT_STREAM_NOTIFY},
  {ZLINK_STREAM_OPT_RECV_MODE, ZLINK_INTERNAL_OPT_STREAM_RECV_MODE},
};

const option_descriptor_t pub_option_table[] = {
  {ZLINK_PUB_OPT_VERBOSE, ZLINK_INTERNAL_OPT_XPUB_VERBOSE},
  {ZLINK_PUB_OPT_VERBOSER, ZLINK_INTERNAL_OPT_XPUB_VERBOSER},
  {ZLINK_PUB_OPT_MANUAL, ZLINK_INTERNAL_OPT_XPUB_MANUAL},
  {ZLINK_PUB_OPT_MANUAL_LAST_VALUE, ZLINK_INTERNAL_OPT_XPUB_MANUAL_LAST_VALUE},
  {ZLINK_PUB_OPT_NODROP, ZLINK_INTERNAL_OPT_XPUB_NODROP},
  {ZLINK_PUB_OPT_WELCOME_MSG, ZLINK_INTERNAL_OPT_XPUB_WELCOME_MSG},
  {ZLINK_PUB_OPT_TOPICS_COUNT, ZLINK_INTERNAL_OPT_TOPICS_COUNT},
  {ZLINK_PUB_OPT_APPROVE_SUBSCRIBE, ZLINK_INTERNAL_OPT_SUBSCRIBE},
  {ZLINK_PUB_OPT_REJECT_SUBSCRIBE, ZLINK_INTERNAL_OPT_UNSUBSCRIBE},
};

const option_descriptor_t sub_option_table[] = {
  {ZLINK_SUB_OPT_TOPICS_COUNT, ZLINK_INTERNAL_OPT_TOPICS_COUNT},
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
