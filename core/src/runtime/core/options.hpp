/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_OPTIONS_HPP_INCLUDED__
#define __ZLINK_OPTIONS_HPP_INCLUDED__

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <memory>

#include "utils/atomic_ptr.hpp"
#include "stddef.h"
#include "utils/stdint.hpp"
#include "core/internal_defs.hpp"
#include "transports/tcp/tcp_address.hpp"

#if defined ZLINK_HAVE_SO_PEERCRED || defined ZLINK_HAVE_LOCAL_PEERCRED
#include <set>
#include <sys/types.h>
#endif
#ifdef ZLINK_HAVE_LOCAL_PEERCRED
#include <sys/ucred.h>
#endif

#if __cplusplus >= 201103L || (defined _MSC_VER && _MSC_VER >= 1700)
#include <type_traits>
#endif

namespace zlink
{
//  Inclusive upper bound of the peer selection weight. The public DEALER and
//  ROUTER weight options, the peer-weight command decoder and the outbound
//  load balancer all share this single range.
const uint32_t max_peer_weight = 10000;

enum transport_lane_t
{
    transport_lane_application = 0,
    transport_lane_completion = 1
};

struct transport_pair_state_t
{
    transport_pair_state_t () :
        generation (1),
        ready_lanes (0),
        reset_active (false),
        reconnect_enabled (true)
    {
    }

    uint64_t begin_reset ()
    {
        bool expected = false;
        if (reset_active.compare_exchange_strong (expected, true,
                                                  std::memory_order_acq_rel)) {
            uint64_t current = generation.load (std::memory_order_relaxed);
            uint64_t next = current == UINT64_MAX ? 1 : current + 1;
            generation.store (next, std::memory_order_release);
            ready_lanes.store (0, std::memory_order_release);
            return next;
        }
        return generation.load (std::memory_order_acquire);
    }

    uint64_t current_generation () const
    {
        return generation.load (std::memory_order_acquire);
    }

    void mark_ready (transport_lane_t lane_, uint64_t generation_)
    {
        if (generation_ != current_generation ())
            return;
        const unsigned int lane_bit =
          lane_ == transport_lane_completion ? 2u : 1u;
        const unsigned int ready =
          ready_lanes.fetch_or (lane_bit, std::memory_order_acq_rel) | lane_bit;
        if (ready == 3u)
            reset_active.store (false, std::memory_order_release);
    }

    void disable_reconnect ()
    {
        reconnect_enabled.store (false, std::memory_order_release);
    }

    bool can_reconnect () const
    {
        return reconnect_enabled.load (std::memory_order_acquire);
    }

    std::atomic<uint64_t> generation;
    std::atomic<unsigned int> ready_lanes;
    std::atomic<bool> reset_active;
    std::atomic<bool> reconnect_enabled;
};

struct options_t
{
    options_t ();

    int setsockopt (int option_, const void *optval_, size_t optvallen_);
    int getsockopt (int option_, void *optval_, size_t *optvallen_) const;

    //  High-water marks for message pipes, in bytes. 0 is unlimited.
    uint64_t sndhwm;
    uint64_t rcvhwm;

    // Raw auto-HWM planning-unit byte override. 0 means socket-type default.
    uint64_t auto_hwm_msg_unit_bytes;

    //  I/O thread affinity.
    uint64_t affinity;

    //  Socket routing id.
    unsigned char routing_id_size;
    unsigned char routing_id[256];

    //  Maximum transfer rate [kb/s]. Default 100kb/s.
    int rate;

    //  Reliability time interval [ms]. Default 10 seconds.
    int recovery_ivl;

    // Sets the time-to-live field in every multicast packet sent.
    int multicast_hops;

    // Sets the maximum transport data unit size in every multicast
    // packet sent.
    int multicast_maxtpdu;

    // SO_SNDBUF and SO_RCVBUF to be passed to underlying transport sockets.
    int sndbuf;
    int rcvbuf;

    // Type of service (containing DSCP and ECN socket options)
    int tos;

    // Protocol-defined priority
    int priority;

    //  Socket type.
    int8_t type;

    //  Linger time, in milliseconds.
    atomic_value_t linger;

    //  Maximum interval in milliseconds beyond which userspace will
    //  timeout connect().
    //  Default 0 (unused)
    int connect_timeout;

    //  Maximum interval in milliseconds beyond which TCP will timeout
    //  retransmitted packets.
    //  Default 0 (unused)
    int tcp_maxrt;

    //  Minimum interval between attempts to reconnect, in milliseconds.
    //  Default 100ms
    int reconnect_ivl;

    //  Maximum interval between attempts to reconnect, in milliseconds.
    //  Default 0ms (meaning maximum interval is disabled)
    int reconnect_ivl_max;

    //  Maximum backlog for pending connections.
    int backlog;

    //  Maximal size of message to handle.
    int64_t maxmsgsize;

    // The timeout for send/recv operations for this socket, in milliseconds.
    int rcvtimeo;
    int sndtimeo;

    // Submit retry policy for transient local send failures.
    int submit_retry_mode;
    int submit_retry_timeout;
    int submit_retry_attempts;

    //  If true, IPv6 is enabled (as well as IPv4)
    bool ipv6;

    //  If 1, connecting pipes are not attached immediately, meaning a send()
    //  on a socket with only connecting pipes would block
    int immediate;

    //  If 1, (X)SUB socket should filter the messages. If 0, it should not.
    bool filter;

    //  If true, the subscription matching on (X)PUB and (X)SUB sockets
    //  is reversed. Messages are sent to and received by non-matching
    //  sockets.
    bool invert_matching;

    //  If true, the routing id message is forwarded to the socket.
    bool recv_routing_id;

    //  For STREAM sockets, emit 0-byte connect/disconnect notifications
    //  into the receive path.
    bool stream_notify;

    //  TCP keep-alive settings.
    //  Defaults to -1 = do not change socket options
    int tcp_keepalive;
    int tcp_keepalive_cnt;
    int tcp_keepalive_idle;
    int tcp_keepalive_intvl;

    //  TCP_NODELAY setting.
    //  1 = enable, 0 = disable, -1 = do not change socket option.
    int tcp_nodelay;

    // TCP accept() filters
    typedef std::vector<tcp_address_mask_t> tcp_accept_filters_t;
    tcp_accept_filters_t tcp_accept_filters;

    // IPC accept() filters
#if defined ZLINK_HAVE_SO_PEERCRED || defined ZLINK_HAVE_LOCAL_PEERCRED
    typedef std::set<uid_t> ipc_uid_accept_filters_t;
    ipc_uid_accept_filters_t ipc_uid_accept_filters;
    typedef std::set<gid_t> ipc_gid_accept_filters_t;
    ipc_gid_accept_filters_t ipc_gid_accept_filters;
#endif
#if defined ZLINK_HAVE_SO_PEERCRED
    typedef std::set<pid_t> ipc_pid_accept_filters_t;
    ipc_pid_accept_filters_t ipc_pid_accept_filters;
#endif

    //  Enable READY properties for ZMP (default: false)
    bool zmp_metadata;

    // Internal request/reply connection-pair metadata.
    transport_lane_t transport_lane;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    bool transport_pair_initiator;
    std::shared_ptr<transport_pair_state_t> transport_pair_state;

    //  ID of the socket.
    int socket_id;

    //  If true, socket conflates outgoing/incoming messages.
    //  Applicable to dealer, pub/sub socket types.
    //  Cannot receive multi-part messages.
    //  Ignores hwm
    bool conflate;

    //  If connection handshake is not done after this many milliseconds,
    //  close socket.  Default is 30 secs.  0 means no handshake timeout.
    int handshake_ivl;

    bool connected;

    // Device to bind the underlying socket to, eg. VRF or interface
    std::string bound_device;

    //  Maximal batching size for engines with receiving functionality.
    //  So, if there are 10 messages that fit into the batch size, all of
    //  them may be read by a single 'recv' system call, thus avoiding
    //  unnecessary network stack traversals.
    int in_batch_size;
    //  Maximal batching size for engines with sending functionality.
    //  So, if there are 10 messages that fit into the batch size, all of
    //  them may be written by a single 'send' system call, thus avoiding
    //  unnecessary network stack traversals.
    int out_batch_size;

    // Use zero copy strategy for storing message content when decoding.
    bool zero_copy;

    // Core-socket-owned monitor emission policy.
    // This stays in the shared bag, but monitor sequencing interprets it.
    int monitor_event_version;

    // Protocol-owned handshake payloads.
    // Session/pipe owners interpret these rather than the central bag.
    std::vector<unsigned char> hello_msg;
    bool can_send_hello_msg;

    std::vector<unsigned char> disconnect_msg;
    bool can_recv_disconnect_msg;

    std::vector<unsigned char> hiccup_msg;
    bool can_recv_hiccup_msg;

    // Transport/network-owned OS socket tuning.
    // Transport setup applies it; options_t only stores the chosen value.
    int busy_poll;

    // Duplicate peer routing id policy for sockets that can observe peer ids.
    int rid_duplicate_policy;

    // Local peer selection weight advertised to connected peers.
    int peer_weight;

#ifdef ZLINK_HAVE_TLS
    //  TLS protocol options
    std::string tls_cert;        // Server certificate file path
    std::string tls_key;         // Server private key file path
    std::string tls_ca;          // CA certificate file path
    int tls_verify;              // Verify client certificate (default: 1)
    int tls_require_client_cert; // Require client certificate for mTLS (default: 0)
    std::string tls_hostname;    // SNI + hostname verification
    int tls_trust_system;        // Use system CA store (default: 1)
    std::string tls_password;    // Private key password (optional)
#endif
};

inline bool get_effective_conflate_option (const options_t &options)
{
    // conflate is only effective for some socket types
    return options.conflate
           && (options.type == ZLINK_CORE_SOCKET_DEALER || options.type == ZLINK_CORE_SOCKET_PUB
               || options.type == ZLINK_CORE_SOCKET_SUB);
}

int do_getsockopt (void *optval_, size_t *optvallen_, const void *value_, size_t value_len_);

template <typename T> int do_getsockopt (void *const optval_, size_t *const optvallen_, T value_)
{
#if __cplusplus >= 201103L && (!defined(__GNUC__) || __GNUC__ > 5)
    static_assert (std::is_trivially_copyable<T>::value, "invalid use of do_getsockopt");
#endif
    return do_getsockopt (optval_, optvallen_, &value_, sizeof (T));
}

int do_getsockopt (void *optval_, size_t *optvallen_, const std::string &value_);

int do_setsockopt_int_as_bool_strict (const void *optval_, size_t optvallen_, bool *out_value_);

int do_setsockopt_int_as_bool_relaxed (const void *optval_, size_t optvallen_, bool *out_value_);
}

#endif
