/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal.sockets;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** Pre-defined socket option keys for use with the typed socket options facades. */
public final class SocketOptions {
    private static final int MAX_ROUTING_ID_BYTES = 256;
    private static final int MAX_LAST_ENDPOINT_BYTES = 512;

    public static final SocketOptionKey<Long> AFFINITY =
      SocketOptionKey.int64("AFFINITY", SocketOption.AFFINITY,
        true, true);

    public static final SocketOptionKey<String> ROUTING_ID =
      SocketOptionKey.string("ROUTING_ID", SocketOption.ROUTING_ID,
        true, true, MAX_ROUTING_ID_BYTES);
    public static final SocketOptionKey<byte[]> ROUTING_ID_BYTES =
      SocketOptionKey.bytes("ROUTING_ID_BYTES", SocketOption.ROUTING_ID,
        true, true, MAX_ROUTING_ID_BYTES);

    public static final SocketOptionKey<Integer> RATE =
      SocketOptionKey.int32("RATE", SocketOption.RATE, true, true);
    public static final SocketOptionKey<Integer> RECOVERY_IVL =
      SocketOptionKey.int32("RECOVERY_IVL", SocketOption.RECOVERY_IVL,
        true, true);
    public static final SocketOptionKey<Integer> SNDBUF =
      SocketOptionKey.int32("SNDBUF", SocketOption.SNDBUF, true, true);
    public static final SocketOptionKey<Integer> RCVBUF =
      SocketOptionKey.int32("RCVBUF", SocketOption.RCVBUF, true, true);
    public static final SocketOptionKey<Integer> FD =
      SocketOptionKey.int32("FD", SocketOption.FD, true, false);
    public static final SocketOptionKey<Integer> EVENTS =
      SocketOptionKey.int32("EVENTS", SocketOption.EVENTS,
        true, false);
    public static final SocketOptionKey<Integer> TYPE =
      SocketOptionKey.int32("TYPE", SocketOption.TYPE, true, false);
    public static final SocketOptionKey<Integer> LINGER =
      SocketOptionKey.int32("LINGER", SocketOption.LINGER, true, true);
    public static final SocketOptionKey<Integer> RECONNECT_IVL =
      SocketOptionKey.int32("RECONNECT_IVL", SocketOption.RECONNECT_IVL,
        true, true);
    public static final SocketOptionKey<Integer> BACKLOG =
      SocketOptionKey.int32("BACKLOG", SocketOption.BACKLOG,
        true, true);
    public static final SocketOptionKey<Integer> RECONNECT_IVL_MAX =
      SocketOptionKey.int32("RECONNECT_IVL_MAX", SocketOption.RECONNECT_IVL_MAX,
        true, true);

    public static final SocketOptionKey<Long> MAXMSGSIZE =
      SocketOptionKey.int64("MAXMSGSIZE", SocketOption.MAXMSGSIZE,
        true, true);

    public static final SocketOptionKey<Long> SNDHWM =
      SocketOptionKey.uint64("SNDHWM", SocketOption.SNDHWM, true, true);
    public static final SocketOptionKey<Long> RCVHWM =
      SocketOptionKey.uint64("RCVHWM", SocketOption.RCVHWM, true, true);
    public static final SocketOptionKey<Integer> MULTICAST_HOPS =
      SocketOptionKey.int32("MULTICAST_HOPS", SocketOption.MULTICAST_HOPS,
        true, true);
    public static final SocketOptionKey<Integer> RCVTIMEO =
      SocketOptionKey.int32("RCVTIMEO", SocketOption.RCVTIMEO,
        true, true);
    public static final SocketOptionKey<Integer> SNDTIMEO =
      SocketOptionKey.int32("SNDTIMEO", SocketOption.SNDTIMEO,
        true, true);

    public static final SocketOptionKey<String> LAST_ENDPOINT =
      SocketOptionKey.string("LAST_ENDPOINT", SocketOption.LAST_ENDPOINT,
        true, false, MAX_LAST_ENDPOINT_BYTES);

    public static final SocketOptionKey<Integer> ROUTER_MANDATORY =
      SocketOptionKey.int32("ROUTER_MANDATORY", SocketOption.ROUTER_MANDATORY,
        true, true);
    public static final SocketOptionKey<Integer> TCP_KEEPALIVE =
      SocketOptionKey.int32("TCP_KEEPALIVE", SocketOption.TCP_KEEPALIVE,
        true, true);
    public static final SocketOptionKey<Integer> TCP_KEEPALIVE_CNT =
      SocketOptionKey.int32("TCP_KEEPALIVE_CNT", SocketOption.TCP_KEEPALIVE_CNT,
        true, true);
    public static final SocketOptionKey<Integer> TCP_KEEPALIVE_IDLE =
      SocketOptionKey.int32("TCP_KEEPALIVE_IDLE", SocketOption.TCP_KEEPALIVE_IDLE,
        true, true);
    public static final SocketOptionKey<Integer> TCP_KEEPALIVE_INTVL =
      SocketOptionKey.int32("TCP_KEEPALIVE_INTVL", SocketOption.TCP_KEEPALIVE_INTVL,
        true, true);
    public static final SocketOptionKey<Integer> TCP_NODELAY =
      SocketOptionKey.int32("TCP_NODELAY", SocketOption.TCP_NODELAY,
        true, true);
    public static final SocketOptionKey<Integer> IMMEDIATE =
      SocketOptionKey.int32("IMMEDIATE", SocketOption.IMMEDIATE,
        true, true);
    public static final SocketOptionKey<Integer> RID_DUPLICATE_POLICY =
      SocketOptionKey.int32("RID_DUPLICATE_POLICY",
        SocketOption.RID_DUPLICATE_POLICY, true, true);
    public static final SocketOptionKey<Integer> ROUTE_VALUE_MAX_SIZE =
      SocketOptionKey.int32("ROUTE_VALUE_MAX_SIZE",
        SocketOption.ROUTE_VALUE_MAX_SIZE, true, true);
    public static final SocketOptionKey<Integer> SUBMIT_RETRY_MODE =
      SocketOptionKey.int32("SUBMIT_RETRY_MODE",
        SocketOption.SUBMIT_RETRY_MODE, true, true);
    public static final SocketOptionKey<Integer> SUBMIT_RETRY_TIMEOUT =
      SocketOptionKey.int32("SUBMIT_RETRY_TIMEOUT",
        SocketOption.SUBMIT_RETRY_TIMEOUT, true, true);
    public static final SocketOptionKey<Integer> SUBMIT_RETRY_ATTEMPTS =
      SocketOptionKey.int32("SUBMIT_RETRY_ATTEMPTS",
        SocketOption.SUBMIT_RETRY_ATTEMPTS, true, true);
    public static final SocketOptionKey<Integer> XPUB_VERBOSE =
      SocketOptionKey.int32("XPUB_VERBOSE", SocketOption.XPUB_VERBOSE,
        true, true);
    public static final SocketOptionKey<Integer> IPV6 =
      SocketOptionKey.int32("IPV6", SocketOption.IPV6, true, true);
    public static final SocketOptionKey<Integer> PROBE_ROUTER =
      SocketOptionKey.int32("PROBE_ROUTER", SocketOption.PROBE_ROUTER,
        true, true);
    public static final SocketOptionKey<Integer> CONFLATE =
      SocketOptionKey.int32("CONFLATE", SocketOption.CONFLATE,
        true, true);
    public static final SocketOptionKey<Integer> TOS =
      SocketOptionKey.int32("TOS", SocketOption.TOS, true, true);

    public static final SocketOptionKey<String> CONNECT_ROUTING_ID =
      SocketOptionKey.string("CONNECT_ROUTING_ID", SocketOption.CONNECT_ROUTING_ID,
        false, true, 0);
    public static final SocketOptionKey<byte[]> CONNECT_ROUTING_ID_BYTES =
      SocketOptionKey.bytes("CONNECT_ROUTING_ID_BYTES", SocketOption.CONNECT_ROUTING_ID,
        false, true, 0);

    public static final SocketOptionKey<Integer> HANDSHAKE_IVL =
      SocketOptionKey.int32("HANDSHAKE_IVL", SocketOption.HANDSHAKE_IVL,
        true, true);
    public static final SocketOptionKey<Integer> XPUB_NODROP =
      SocketOptionKey.int32("XPUB_NODROP", SocketOption.XPUB_NODROP,
        true, true);
    public static final SocketOptionKey<Integer> BLOCKY =
      SocketOptionKey.int32("BLOCKY", SocketOption.BLOCKY,
        true, true);
    public static final SocketOptionKey<Integer> XPUB_MANUAL =
      SocketOptionKey.int32("XPUB_MANUAL", SocketOption.XPUB_MANUAL,
        true, true);

    public static final SocketOptionKey<String> XPUB_WELCOME_MSG =
      SocketOptionKey.string("XPUB_WELCOME_MSG", SocketOption.XPUB_WELCOME_MSG,
        false, true, 0);
    public static final SocketOptionKey<byte[]> XPUB_WELCOME_MSG_BYTES =
      SocketOptionKey.bytes("XPUB_WELCOME_MSG_BYTES", SocketOption.XPUB_WELCOME_MSG,
        false, true, 0);
    public static final SocketOptionKey<byte[]> PUB_APPROVE_SUBSCRIBE_BYTES =
      SocketOptionKey.bytes("PUB_APPROVE_SUBSCRIBE_BYTES",
        SocketOption.PUB_APPROVE_SUBSCRIBE, false, true, 0);
    public static final SocketOptionKey<byte[]> PUB_REJECT_SUBSCRIBE_BYTES =
      SocketOptionKey.bytes("PUB_REJECT_SUBSCRIBE_BYTES",
        SocketOption.PUB_REJECT_SUBSCRIBE, false, true, 0);

    public static final SocketOptionKey<Integer> STREAM_NOTIFY =
      SocketOptionKey.int32("STREAM_NOTIFY", SocketOption.STREAM_NOTIFY,
        true, true);
    public static final SocketOptionKey<Integer> INVERT_MATCHING =
      SocketOptionKey.int32("INVERT_MATCHING", SocketOption.INVERT_MATCHING,
        true, true);
    public static final SocketOptionKey<Integer> XPUB_VERBOSER =
      SocketOptionKey.int32("XPUB_VERBOSER", SocketOption.XPUB_VERBOSER,
        true, true);
    public static final SocketOptionKey<Integer> CONNECT_TIMEOUT =
      SocketOptionKey.int32("CONNECT_TIMEOUT", SocketOption.CONNECT_TIMEOUT,
        true, true);
    public static final SocketOptionKey<Integer> TCP_MAXRT =
      SocketOptionKey.int32("TCP_MAXRT", SocketOption.TCP_MAXRT,
        true, true);
    public static final SocketOptionKey<Integer> MULTICAST_MAXTPDU =
      SocketOptionKey.int32("MULTICAST_MAXTPDU", SocketOption.MULTICAST_MAXTPDU,
        true, true);
    public static final SocketOptionKey<Integer> USE_FD =
      SocketOptionKey.int32("USE_FD", SocketOption.USE_FD,
        true, true);

    public static final SocketOptionKey<String> BINDTODEVICE =
      SocketOptionKey.string("BINDTODEVICE", SocketOption.BINDTODEVICE,
        true, true, MAX_ROUTING_ID_BYTES);
    public static final SocketOptionKey<String> TLS_CERT =
      SocketOptionKey.string("TLS_CERT", SocketOption.TLS_CERT,
        false, true, 0);
    public static final SocketOptionKey<String> TLS_KEY =
      SocketOptionKey.string("TLS_KEY", SocketOption.TLS_KEY,
        false, true, 0);
    public static final SocketOptionKey<String> TLS_CA =
      SocketOptionKey.string("TLS_CA", SocketOption.TLS_CA,
        false, true, 0);
    public static final SocketOptionKey<Integer> TLS_VERIFY =
      SocketOptionKey.int32("TLS_VERIFY", SocketOption.TLS_VERIFY,
        true, true);
    public static final SocketOptionKey<Integer> TLS_REQUIRE_CLIENT_CERT =
      SocketOptionKey.int32("TLS_REQUIRE_CLIENT_CERT", SocketOption.TLS_REQUIRE_CLIENT_CERT,
        true, true);
    public static final SocketOptionKey<String> TLS_HOSTNAME =
      SocketOptionKey.string("TLS_HOSTNAME", SocketOption.TLS_HOSTNAME,
        false, true, 0);
    public static final SocketOptionKey<Integer> TLS_TRUST_SYSTEM =
      SocketOptionKey.int32("TLS_TRUST_SYSTEM", SocketOption.TLS_TRUST_SYSTEM,
        true, true);
    public static final SocketOptionKey<String> TLS_PASSWORD =
      SocketOptionKey.string("TLS_PASSWORD", SocketOption.TLS_PASSWORD,
        false, true, 0);

    public static final SocketOptionKey<Integer> XPUB_MANUAL_LAST_VALUE =
      SocketOptionKey.int32("XPUB_MANUAL_LAST_VALUE", SocketOption.XPUB_MANUAL_LAST_VALUE,
        true, true);
    public static final SocketOptionKey<Integer> TOPICS_COUNT =
      SocketOptionKey.int32("TOPICS_COUNT", SocketOption.TOPICS_COUNT,
        true, false);

    public static final SocketOptionKey<String> ZMP_METADATA =
      SocketOptionKey.string("ZMP_METADATA", SocketOption.ZMP_METADATA,
        false, true, 0);
    public static final SocketOptionKey<byte[]> ZMP_METADATA_BYTES =
      SocketOptionKey.bytes("ZMP_METADATA_BYTES", SocketOption.ZMP_METADATA,
        false, true, 0);

    private static final List<SocketOptionKey<?>> ALL =
      Collections.unmodifiableList(Arrays.asList(
        AFFINITY,
        ROUTING_ID, ROUTING_ID_BYTES,
        RATE, RECOVERY_IVL, SNDBUF, RCVBUF, FD, EVENTS, TYPE,
        LINGER, RECONNECT_IVL, BACKLOG, RECONNECT_IVL_MAX, MAXMSGSIZE,
        SNDHWM, RCVHWM, MULTICAST_HOPS, RCVTIMEO, SNDTIMEO,
        LAST_ENDPOINT, ROUTER_MANDATORY, TCP_KEEPALIVE,
        TCP_KEEPALIVE_CNT, TCP_KEEPALIVE_IDLE, TCP_KEEPALIVE_INTVL,
        TCP_NODELAY, IMMEDIATE, XPUB_VERBOSE, IPV6, PROBE_ROUTER,
        CONFLATE, RID_DUPLICATE_POLICY, ROUTE_VALUE_MAX_SIZE,
        SUBMIT_RETRY_MODE, SUBMIT_RETRY_TIMEOUT, SUBMIT_RETRY_ATTEMPTS, TOS,
        CONNECT_ROUTING_ID, CONNECT_ROUTING_ID_BYTES,
        HANDSHAKE_IVL, XPUB_NODROP, BLOCKY, XPUB_MANUAL,
        XPUB_WELCOME_MSG, XPUB_WELCOME_MSG_BYTES,
        STREAM_NOTIFY, INVERT_MATCHING, XPUB_VERBOSER,
        CONNECT_TIMEOUT, TCP_MAXRT, MULTICAST_MAXTPDU, USE_FD,
        BINDTODEVICE,
        TLS_CERT, TLS_KEY, TLS_CA, TLS_VERIFY,
        TLS_REQUIRE_CLIENT_CERT, TLS_HOSTNAME, TLS_TRUST_SYSTEM,
        TLS_PASSWORD,
        XPUB_MANUAL_LAST_VALUE,
        PUB_APPROVE_SUBSCRIBE_BYTES, PUB_REJECT_SUBSCRIBE_BYTES,
        TOPICS_COUNT,
        ZMP_METADATA, ZMP_METADATA_BYTES
      ));

    private SocketOptions() {
    }

    public static List<SocketOptionKey<?>> all() {
        return ALL;
    }
}
