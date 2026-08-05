/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <string.h>
#include <limits.h>

#include "core/options.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"

static int sockopt_invalid ()
{
#if defined(ZLINK_ACT_MILITANT)
    zlink_assert (false);
#endif
    errno = EINVAL;
    return -1;
}

int zlink::do_getsockopt (void *const optval_, size_t *const optvallen_, const std::string &value_)
{
    return do_getsockopt (optval_, optvallen_, value_.c_str (), value_.size () + 1);
}

int zlink::do_getsockopt (void *const optval_,
                          size_t *const optvallen_,
                          const void *value_,
                          const size_t value_len_)
{
    if (*optvallen_ < value_len_) {
        return sockopt_invalid ();
    }
    memcpy (optval_, value_, value_len_);
    memset (static_cast<char *> (optval_) + value_len_, 0, *optvallen_ - value_len_);
    *optvallen_ = value_len_;
    return 0;
}

template <typename T>
static int do_setsockopt (const void *const optval_, const size_t optvallen_, T *const out_value_)
{
    if (optvallen_ == sizeof (T)) {
        memcpy (out_value_, optval_, sizeof (T));
        return 0;
    }
    return sockopt_invalid ();
}

int zlink::do_setsockopt_int_as_bool_strict (const void *const optval_,
                                             const size_t optvallen_,
                                             bool *const out_value_)
{
    int value = -1;
    if (do_setsockopt (optval_, optvallen_, &value) == -1)
        return -1;
    if (value == 0 || value == 1) {
        *out_value_ = (value != 0);
        return 0;
    }
    return sockopt_invalid ();
}

int zlink::do_setsockopt_int_as_bool_relaxed (const void *const optval_,
                                              const size_t optvallen_,
                                              bool *const out_value_)
{
    int value = -1;
    if (do_setsockopt (optval_, optvallen_, &value) == -1)
        return -1;
    *out_value_ = (value != 0);
    return 0;
}

static int do_setsockopt_string_allow_empty_strict (const void *const optval_,
                                                    const size_t optvallen_,
                                                    std::string *const out_value_,
                                                    const size_t max_len_)
{
    if (optval_ == NULL && optvallen_ == 0) {
        out_value_->clear ();
        return 0;
    }
    if (optval_ != NULL && optvallen_ > 0 && optvallen_ <= max_len_) {
        out_value_->assign (static_cast<const char *> (optval_), optvallen_);
        return 0;
    }
    return sockopt_invalid ();
}

const uint64_t default_hwm_bytes = ZLINK_HWM_BYTES_DFLT;
const int default_batch_size = 8192; // 32768;// //16384;

zlink::options_t::options_t () :
    sndhwm (default_hwm_bytes),
    rcvhwm (default_hwm_bytes),
    auto_hwm_msg_unit_bytes (0),
    affinity (0),
    routing_id_size (0),
    rate (100),
    recovery_ivl (10000),
    multicast_hops (1),
    multicast_maxtpdu (1500),
    sndbuf (-1),
    rcvbuf (-1),
    tos (0),
    priority (0),
    type (-1),
    linger (-1),
    connect_timeout (0),
    tcp_maxrt (0),
    reconnect_ivl (100),
    reconnect_ivl_max (0),
    backlog (100),
    maxmsgsize (-1),
    rcvtimeo (1000),
    sndtimeo (1000),
    submit_retry_mode (ZLINK_SUBMIT_RETRY_OFF),
    submit_retry_timeout (0),
    submit_retry_attempts (0),
    ipv6 (false),
    immediate (0),
    filter (false),
    invert_matching (false),
    recv_routing_id (false),
    stream_notify (false),
    tcp_keepalive (-1),
    tcp_keepalive_cnt (-1),
    tcp_keepalive_idle (-1),
    tcp_keepalive_intvl (-1),
    tcp_nodelay (1),
    zmp_metadata (false),
    transport_lane (transport_lane_application),
    transport_pair_id (0),
    transport_pair_generation (0),
    transport_pair_initiator (false),
    transport_pair_state (),
    socket_id (0),
    conflate (false),
    handshake_ivl (30000),
    connected (false),
    in_batch_size (default_batch_size),
    out_batch_size (default_batch_size),
    zero_copy (true),
    monitor_event_version (1),
    can_send_hello_msg (false),
    can_recv_disconnect_msg (false),
    can_recv_hiccup_msg (false),
    busy_poll (0),
    rid_duplicate_policy (ZLINK_RID_DUPLICATE_REJECT),
    peer_weight (100)
#ifdef ZLINK_HAVE_TLS
    ,
    tls_verify (1),
    tls_require_client_cert (0),
    tls_trust_system (1)
#endif
{
}
