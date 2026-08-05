/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_STREAM_HPP_INCLUDED__
#define __ZLINK_STREAM_HPP_INCLUDED__

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sockets/common/socket_base.hpp"
#include "sockets/internal/fq.hpp"
#include "utils/fast_mutex.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class ctx_t;
class pipe_t;

class stream_t ZLINK_FINAL : public routing_socket_base_t
{
  public:
    typedef void (*session_observer_fn) (void *userdata_,
                                         const zlink_routing_id_t *peer_rid_,
                                         bool connected_);

    enum dispatch_mode_t
    {
        dispatch_mode_none = 0,
        dispatch_mode_raw,
        dispatch_mode_packet
    };

    stream_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~stream_t () ZLINK_OVERRIDE;

    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsend (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xterm_peer_rid (const zlink_routing_id_t *peer_rid_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    int xsocket_msg_dispatch (zlink::msg_t *msg_, zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_FINAL;
    int stream_dispatch_start_raw (zlink_stream_on_raw_fn callback_) ZLINK_OVERRIDE;
    int stream_set_msg_handler_with_userdata (zlink_socket_msg_handler_fn handler_,
                                              void *userdata_) ZLINK_OVERRIDE;
    int stream_set_packet_msg_handler_with_userdata (zlink_stream_packet_handler_fn handler_,
                                                     void *userdata_) ZLINK_OVERRIDE;
    int stream_dispatch_stop () ZLINK_OVERRIDE;
    bool stream_dispatch_active () const ZLINK_OVERRIDE;
    bool stream_dispatch_in_callback () const ZLINK_OVERRIDE;
    uint32_t stream_dispatch_inflight () const ZLINK_OVERRIDE;
    int stream_dispatch_send_from_io (const zlink_routing_id_t *rid_,
                                      const void *data_,
                                      size_t size_,
                                      int flags_) ZLINK_OVERRIDE;
    int stream_dispatch_send_msg_from_io (const zlink_routing_id_t *rid_,
                                          zlink::msg_t *msg_,
                                          int flags_) ZLINK_OVERRIDE;
    int stream_dispatch_send_current_msg_from_io (zlink::msg_t *msg_, int flags_) ZLINK_OVERRIDE;
    std::recursive_mutex *api_sync_mutex () ZLINK_OVERRIDE;

    //  Internal session observation. These methods expose only live
    //  transport membership inside Core; they are not public socket APIs.
    void peer_routing_ids (std::vector<zlink_routing_id_t> *out_);
    void set_session_observer (session_observer_fn observer_, void *userdata_);
    void clear_session_observer (void *userdata_);

  private:
    enum
    {
        route_shard_count = 64
    };

    struct route_shard_t
    {
        typedef std::map<uint32_t, zlink::pipe_t *> routes_t;

        fast_mutex_t sync;
        routes_t routes;
    };

    route_shard_t &route_shard_for (uint32_t routing_id_);
    void identify_peer (pipe_t *pipe_, bool locally_initiated_);
    uint32_t ensure_dispatch_routing_id (pipe_t *pipe_);
    void maybe_emit_connect_event (pipe_t *pipe_, uint32_t routing_id_value_ = 0);
    void notify_session_observer (uint32_t routing_id_, bool connected_);
    int xstream_dispatch_msg (zlink::msg_t *msg_, zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    int stream_dispatch_packet_msg_from_io (const zlink_routing_id_t *rid_,
                                            zlink::msg_t *msg_,
                                            zlink::pipe_t *pipe_);
    int stream_dispatch_raw_msg_from_io (const zlink_routing_id_t *rid_, zlink::msg_t *msg_);
    uint32_t resolve_dispatch_routing_id_fast (const zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    void stop_dispatch_from_callback ();
    void clear_packet_dispatch_state ();
    bool stream_dispatch_owns_tls () const;
    fq_t _fq;

    bool _prefetched;
    bool _routing_id_sent;
    zlink::msg_t _prefetched_id;
    zlink::msg_t _prefetched_msg;
    zlink::pipe_t *_current_in;
    bool _more_in;

    zlink::pipe_t *_current_out;
    bool _more_out;

    std::atomic<uint32_t> _next_integral_routing_id;
    route_shard_t _route_shards[route_shard_count];

    std::atomic<bool> _dispatch_active;
    std::atomic<dispatch_mode_t> _dispatch_mode;
    std::atomic<uint32_t> _dispatch_inflight;
    std::atomic<zlink_stream_on_raw_fn> _dispatch_raw_callback;
    std::atomic<zlink_socket_msg_handler_fn> _dispatch_msg_handler;
    std::atomic<void *> _dispatch_msg_handler_userdata;
    std::atomic<zlink_stream_packet_handler_fn> _dispatch_packet_handler;
    std::atomic<void *> _dispatch_packet_handler_userdata;
    mutable std::recursive_mutex _api_mutex;
    std::mutex _session_observer_mutex;
    session_observer_fn _session_observer;
    void *_session_observer_userdata;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (stream_t)
};
}

#endif
