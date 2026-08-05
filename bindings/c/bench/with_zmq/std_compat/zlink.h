#ifndef BENCH_WITH_ZMQ_STD_COMPAT_ZLINK_H
#define BENCH_WITH_ZMQ_STD_COMPAT_ZLINK_H

#include <zmq.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#endif

typedef int zlink_socket_type_t;
typedef int zlink_option_t;
typedef int zlink_ctx_option_t;
typedef int zlink_pub_option_t;
typedef int zlink_config_result_t;
typedef int zlink_submit_result_t;
typedef int zlink_request_result_t;
typedef int zlink_recv_result_t;
typedef int zlink_handler_result_t;
typedef int zlink_close_result_t;
typedef int zlink_bind_result_t;
typedef int zlink_connect_result_t;
typedef int zlink_send_flags_t;
typedef int zlink_recv_flags_t;
typedef uint64_t zlink_socket_monitor_event_mask_t;
typedef zmq_msg_t zlink_msg_t;
typedef enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,
    ZLINK_PART_MORE = 1
} zlink_part_flag_t;

enum
{
    ZLINK_CONFIG_OK = 0,
    ZLINK_CONFIG_INVALID_ARGUMENT = 702,
    ZLINK_SUBMIT_OK = 0,
    ZLINK_SUBMIT_BACKPRESSURED = 1,
    ZLINK_CLOSE_OK = 0,
    ZLINK_BIND_OK = 0,
    ZLINK_CONNECT_OK = 0,
    ZLINK_RECV_OK = 0,
    ZLINK_HANDLER_OK = 0,
    ZLINK_SEND_FLAGS_NONE = 0,
    ZLINK_SEND_FLAGS_DONTWAIT = 1,
    ZLINK_RECV_FLAGS_NONE = 0,
    ZLINK_RECV_FLAGS_DONTWAIT = 1
};
#if defined(_WIN32)
typedef SOCKET zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

namespace zlink_std_compat
{
static const size_t k_recv_frame_cap = 8;

inline std::vector<zlink_msg_t> &recv_tls_parts ()
{
    static thread_local std::vector<zlink_msg_t> parts (2);
    static thread_local bool initialized = false;
    if (!initialized) {
        for (size_t i = 0; i < parts.size (); ++i)
            zmq_msg_init (&parts[i]);
        initialized = true;
    }
    return parts;
}

inline std::vector<unsigned char> &recv_tls_occupied ()
{
    static thread_local std::vector<unsigned char> occupied (2, 0);
    return occupied;
}

inline size_t &recv_tls_count ()
{
    static thread_local size_t count = 0;
    return count;
}

inline size_t &recv_tls_next_index ()
{
    static thread_local size_t index = 0;
    return index;
}

inline uint64_t &recv_tls_request_seq ()
{
    static thread_local uint64_t request_seq = 0;
    return request_seq;
}

inline int recv_tls_push (zlink_msg_t *src_)
{
    std::vector<zlink_msg_t> &parts = recv_tls_parts ();
    std::vector<unsigned char> &occupied = recv_tls_occupied ();
    size_t &count = recv_tls_count ();
    if (count >= parts.size ()) {
        errno = EMSGSIZE;
        return -1;
    }
    if (zmq_msg_move (&parts[count], src_) != 0)
        return -1;
    occupied[count] = 1;
    ++count;
    return 0;
}

inline int recv_tls_take_part (zlink_msg_t *part_out_, zlink_part_flag_t *has_more_out_)
{
    if (!part_out_ || !has_more_out_) {
        errno = EINVAL;
        return -1;
    }

    size_t &count = recv_tls_count ();
    size_t &next_index = recv_tls_next_index ();
    std::vector<zlink_msg_t> &parts = recv_tls_parts ();
    std::vector<unsigned char> &occupied = recv_tls_occupied ();
    if (next_index >= count || !occupied[next_index]) {
        errno = EAGAIN;
        return -1;
    }

    if (zmq_msg_move (part_out_, &parts[next_index]) != 0)
        return -1;

    occupied[next_index] = 0;
    ++next_index;
    *has_more_out_ = next_index < count ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
    if (*has_more_out_ == ZLINK_PART_FINAL) {
        count = 0;
        next_index = 0;
    }
    return 0;
}

inline void close_frame_range (zlink_msg_t *frames_, size_t frame_count_)
{
    if (!frames_)
        return;
    for (size_t i = 0; i < frame_count_; ++i)
        zmq_msg_close (&frames_[i]);
}
}

typedef struct zlink_pollitem_t
{
    void *socket;
    zlink_fd_t fd;
    short events;
    short revents;
} zlink_pollitem_t;

typedef struct zlink_routing_id_t
{
    uint8_t size;
    unsigned char data[256];
} zlink_routing_id_t;

namespace zlink_std_compat
{
inline zlink_routing_id_t &recv_tls_source_rid ()
{
    static thread_local zlink_routing_id_t rid;
    return rid;
}

inline void recv_tls_reset ()
{
    std::vector<zlink_msg_t> &parts = recv_tls_parts ();
    std::vector<unsigned char> &occupied = recv_tls_occupied ();
    size_t &count = recv_tls_count ();
    size_t &next_index = recv_tls_next_index ();
    for (size_t i = 0; i < count; ++i) {
        if (!occupied[i])
            continue;
        zmq_msg_close (&parts[i]);
        zmq_msg_init (&parts[i]);
        occupied[i] = 0;
    }
    count = 0;
    next_index = 0;
    std::memset (&recv_tls_source_rid (), 0, sizeof (zlink_routing_id_t));
    recv_tls_request_seq () = 0;
}
}

typedef struct zlink_monitor_event_t
{
    uint64_t event;
    uint64_t value;
    zlink_routing_id_t routing_id;
    char local_addr[256];
    char remote_addr[256];
} zlink_monitor_event_t;

typedef zlink_monitor_event_t zlink_socket_monitor_event_t;
typedef void (*zlink_socket_monitor_handler_fn) (const zlink_monitor_event_t *event_,
                                                 void *userdata_);
typedef void (*zlink_socket_msg_handler_fn) (const zlink_routing_id_t *source_rid_,
                                             zlink_msg_t *parts_,
                                             size_t part_count_,
                                             void *userdata_);
typedef void (*zlink_send_ready_handler_fn) (void *subject_, void *userdata_);

typedef struct zlink_socket_monitor_open_options_t
{
    zlink_socket_monitor_event_mask_t events;
} zlink_socket_monitor_open_options_t;

typedef struct zlink_monitor_status_t
{
    uint32_t source_kind;
    uint32_t state_flags;
    uint32_t detail_flags;
    uint32_t ready_count;
    uint64_t snd_pending_msgs;
    uint64_t rcv_pending_msgs;
} zlink_monitor_status_t;

typedef struct zlink_poller_event_t
{
    void *socket;
    zlink_fd_t fd;
    void *user_data;
    short events;
} zlink_poller_event_t;

enum
{
    ZLINK_IO_THREADS = 1,
    ZLINK_MAX_SOCKETS = 2,
    ZLINK_CTX_OPT_BLOCKY = 10
};

enum
{
    ZLINK_SOCKET_DEALER = ZMQ_DEALER,
    ZLINK_SOCKET_ROUTER = ZMQ_ROUTER,
    ZLINK_SOCKET_PUB = ZMQ_PUB,
    ZLINK_SOCKET_SUB = ZMQ_SUB,
    ZLINK_SOCKET_STREAM = ZMQ_STREAM
};

#ifndef ZLINK_SOCKET_STREAM
#define ZLINK_SOCKET_STREAM ZMQ_STREAM
#endif

enum
{
    ZLINK_OPT_LINGER = ZMQ_LINGER,
    ZLINK_OPT_SNDHWM = ZMQ_SNDHWM,
    ZLINK_OPT_RCVHWM = ZMQ_RCVHWM,
    ZLINK_OPT_SNDTIMEO = ZMQ_SNDTIMEO,
    ZLINK_OPT_RCVTIMEO = ZMQ_RCVTIMEO,
    ZLINK_OPT_SNDBUF = ZMQ_SNDBUF,
    ZLINK_OPT_RCVBUF = ZMQ_RCVBUF,
    ZLINK_OPT_EVENTS = ZMQ_EVENTS,
    ZLINK_OPT_LAST_ENDPOINT = ZMQ_LAST_ENDPOINT,
    ZLINK_LAST_ENDPOINT = ZMQ_LAST_ENDPOINT,
    ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = ZMQ_CONNECT_ROUTING_ID
};

#ifdef ZMQ_TCP_NODELAY
#define ZLINK_OPT_TCP_NODELAY ZMQ_TCP_NODELAY
#else
#define ZLINK_OPT_TCP_NODELAY 25
#endif
#define ZLINK_OPT_TLS_CERT 95
#define ZLINK_OPT_TLS_KEY 96
#define ZLINK_OPT_TLS_CA 97
#define ZLINK_OPT_TLS_HOSTNAME 100
#define ZLINK_OPT_TLS_TRUST_SYSTEM 101

enum
{
    ZLINK_PUB_OPT_NODROP = 1
};

#ifndef ZLINK_DONTWAIT
#define ZLINK_DONTWAIT ZMQ_DONTWAIT
#endif
#ifndef ZLINK_POLLIN
#define ZLINK_POLLIN ZMQ_POLLIN
#endif
#ifndef ZLINK_POLLOUT
#define ZLINK_POLLOUT ZMQ_POLLOUT
#endif
#ifndef ZLINK_POLLERR
#define ZLINK_POLLERR ZMQ_POLLERR
#endif
#ifndef ZLINK_POLLPRI
#define ZLINK_POLLPRI 8
#endif
#ifndef ZLINK_HAVE_POLLER
#define ZLINK_HAVE_POLLER 1
#endif

enum
{
    ZLINK_EVENT_CONNECTION_READY = 0x1001,
    ZLINK_EVENT_BIND_FAILED = 0x1002,
    ZLINK_EVENT_ACCEPT_FAILED = 0x1003,
    ZLINK_EVENT_CLOSE_FAILED = 0x1004,
    ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL = 0x1005,
    ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL = 0x1006,
    ZLINK_EVENT_HANDSHAKE_FAILED_AUTH = 0x1007,
    ZLINK_EVENT_SUB_DELIVERY_READY_CHANGED = 0x1008,
    ZLINK_EVENT_PUB_DELIVERY_READY_CHANGED = 0x1009
};

static std::atomic<unsigned long> g_zlink_monitor_seq (0);
static std::atomic<void *> g_zlink_last_ctx (NULL);
static std::mutex g_zlink_monitor_registry_sync;
static std::set<void *> g_zlink_monitor_registry;

struct zlink_monitor_handle_t
{
    zlink_monitor_handle_t () :
        owner (NULL),
        monitor (NULL),
        callback (NULL),
        userdata (NULL),
        stop (false),
        ready_count (0),
        socket_type (0)
    {
    }

    void *owner;
    void *monitor;
    zlink_socket_monitor_handler_fn callback;
    void *userdata;
    std::thread worker;
    std::atomic<bool> stop;
    std::atomic<uint32_t> ready_count;
    int socket_type;
};

struct zlink_poller_item_t
{
    void *socket;
    void *user_data;
    short events;
};

struct zlink_poller_handle_t
{
    std::vector<zlink_poller_item_t> items;
};

inline zlink_monitor_handle_t *zlink_monitor_from_handle (void *handle_)
{
    if (!handle_)
        return NULL;
    std::lock_guard<std::mutex> lock (g_zlink_monitor_registry_sync);
    if (g_zlink_monitor_registry.count (handle_) == 0)
        return NULL;
    return static_cast<zlink_monitor_handle_t *> (handle_);
}

inline void *zlink_resolve_socket_handle (void *handle_)
{
    zlink_monitor_handle_t *monitor = zlink_monitor_from_handle (handle_);
    return monitor ? monitor->monitor : handle_;
}

inline int zlink_errno (void)
{
    return zmq_errno ();
}

inline const char *zlink_strerror (int errnum_)
{
    return zmq_strerror (errnum_);
}

inline void *zlink_ctx_new (void)
{
    void *ctx = zmq_ctx_new ();
    g_zlink_last_ctx.store (ctx, std::memory_order_release);
    return ctx;
}

inline int zlink_ctx_term (void *context_)
{
    return zmq_ctx_term (context_);
}

inline int zlink_ctx_shutdown (void *context_)
{
#ifdef ZMQ_BLOCKY
    (void) zmq_ctx_set (context_, ZMQ_BLOCKY, 0);
#endif
    return zmq_ctx_shutdown (context_);
}

inline int zlink_ctx_set (void *context_, zlink_ctx_option_t option_, int optval_)
{
    int zmq_option = option_;
    if (option_ == ZLINK_IO_THREADS)
        zmq_option = ZMQ_IO_THREADS;
#ifdef ZMQ_MAX_SOCKETS
    else if (option_ == ZLINK_MAX_SOCKETS)
        zmq_option = ZMQ_MAX_SOCKETS;
#endif
#ifdef ZMQ_BLOCKY
    else if (option_ == ZLINK_CTX_OPT_BLOCKY)
        return 0;
#endif
    return zmq_ctx_set (context_, zmq_option, optval_);
}

inline void *zlink_socket (void *ctx_, zlink_socket_type_t type_)
{
    g_zlink_last_ctx.store (ctx_, std::memory_order_release);
    return zmq_socket (ctx_, type_);
}

inline int zlink_close (void *socket_)
{
    return zmq_close (socket_);
}

inline int zlink_bind (void *socket_, const char *endpoint_)
{
    return zmq_bind (socket_, endpoint_);
}

inline int zlink_connect (void *socket_, const char *endpoint_)
{
    return zmq_connect (socket_, endpoint_);
}

inline int zlink_has (const char *capability_)
{
    return zmq_has (capability_);
}

inline int
zlink_set_option (void *socket_, zlink_option_t option_, const void *optval_, size_t optvallen_)
{
    return zmq_setsockopt (zlink_resolve_socket_handle (socket_), option_, optval_, optvallen_);
}

inline int
zlink_get_option (void *socket_, zlink_option_t option_, void *optval_, size_t *optvallen_)
{
    return zmq_getsockopt (zlink_resolve_socket_handle (socket_), option_, optval_, optvallen_);
}

inline int zlink_set_pub_option (void *socket_,
                                 zlink_pub_option_t option_,
                                 const void *optval_,
                                 size_t optvallen_)
{
    (void) socket_;
    (void) option_;
    (void) optval_;
    (void) optvallen_;
    return 0;
}

inline int zlink_set_subscription (void *handle_, const char *filter_)
{
    return zmq_setsockopt (handle_, ZMQ_SUBSCRIBE, filter_ ? filter_ : "",
                           filter_ ? std::strlen (filter_) : 0);
}

inline int zlink_unset_subscription (void *handle_, const char *filter_)
{
    return zmq_setsockopt (handle_, ZMQ_UNSUBSCRIBE, filter_ ? filter_ : "",
                           filter_ ? std::strlen (filter_) : 0);
}

inline int zlink_subscription_at (
  void *handle_, size_t index_, char *filter_out_, size_t *filter_len_inout_, int *is_pattern_out_)
{
    (void) handle_;
    (void) index_;
    (void) filter_out_;
    (void) filter_len_inout_;
    (void) is_pattern_out_;
    errno = ENOTSUP;
    return -1;
}

inline int zlink_set_routing_id (void *socket_, const void *data_, size_t size_)
{
    return zmq_setsockopt (socket_, ZMQ_ROUTING_ID, data_, size_);
}

inline int
zlink_set_router_option (void *socket_, int option_, const void *optval_, size_t optvallen_)
{
    return zmq_setsockopt (socket_, option_, optval_, optvallen_);
}

inline int zlink_msg_init (zlink_msg_t *msg_)
{
    return zmq_msg_init (msg_);
}

inline int zlink_msg_init_size (zlink_msg_t *msg_, size_t size_)
{
    return zmq_msg_init_size (msg_, size_);
}

inline int
zlink_msg_init_data (zlink_msg_t *msg_, void *data_, size_t size_, zmq_free_fn *ffn_, void *hint_)
{
    return zmq_msg_init_data (msg_, data_, size_, ffn_, hint_);
}

inline int zlink_msg_close (zlink_msg_t *msg_)
{
    return zmq_msg_close (msg_);
}

inline int zlink_msg_move (zlink_msg_t *dest_, zlink_msg_t *src_)
{
    return zmq_msg_move (dest_, src_);
}

inline void *zlink_msg_data (zlink_msg_t *msg_)
{
    return zmq_msg_data (msg_);
}

inline size_t zlink_msg_size (zlink_msg_t *msg_)
{
    return zmq_msg_size (msg_);
}

inline void zlink_multipart_close (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < part_count_; ++i)
        zmq_msg_close (&parts_[i]);
}

inline int zlink_poll (zlink_pollitem_t *items_,
                       int nitems_,
                       long timeout_,
                       zlink_config_result_t *error_out_ = NULL)
{
    std::vector<zmq_pollitem_t> raw_items (nitems_);
    for (int i = 0; i < nitems_; ++i) {
        raw_items[i].socket = zlink_resolve_socket_handle (items_[i].socket);
        raw_items[i].fd = items_[i].fd;
        raw_items[i].events = items_[i].events;
        raw_items[i].revents = 0;
    }
    const int rc = zmq_poll (&raw_items[0], nitems_, timeout_);
    if (rc >= 0) {
        for (int i = 0; i < nitems_; ++i)
            items_[i].revents = raw_items[i].revents;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_OK;
    } else if (error_out_) {
        *error_out_ = ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    return rc;
}

inline int
zlink_std_compat_send (void *s_, zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        const int flags = static_cast<int> (flags_ | ((i + 1 < part_count_) ? ZMQ_SNDMORE : 0));
        if (zmq_msg_send (&parts_[i], s_, flags) < 0)
            return -1;
    }
    return 0;
}

inline int zlink_std_compat_send_rid (void *s_,
                                      const zlink_routing_id_t *target_rid_,
                                      zlink_msg_t *parts_,
                                      size_t part_count_,
                                      zlink_send_flags_t flags_)
{
    if (!target_rid_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink_msg_t rid_msg;
    if (zmq_msg_init_size (&rid_msg, target_rid_->size) != 0)
        return -1;
    if (target_rid_->size > 0) {
        std::memcpy (zmq_msg_data (&rid_msg), target_rid_->data, target_rid_->size);
    }
    if (zmq_msg_send (&rid_msg, s_, static_cast<int> (flags_ | ZMQ_SNDMORE)) < 0) {
        zmq_msg_close (&rid_msg);
        return -1;
    }
    return zlink_std_compat_send (s_, parts_, part_count_, flags_);
}

inline int zlink_std_compat_recv (void *s_,
                                  zlink_routing_id_t *source_rid_out_,
                                  zlink_msg_t **parts_out_,
                                  size_t *part_count_out_,
                                  zlink_send_flags_t flags_)
{
    if (!parts_out_ || !part_count_out_) {
        errno = EINVAL;
        return -1;
    }

    zlink_std_compat::recv_tls_reset ();
    *parts_out_ = NULL;
    *part_count_out_ = 0;
    if (source_rid_out_)
        source_rid_out_->size = 0;
    std::memset (&zlink_std_compat::recv_tls_source_rid (), 0, sizeof (zlink_routing_id_t));
    zlink_std_compat::recv_tls_request_seq () = 0;

    int socket_type = 0;
    size_t socket_type_size = sizeof (socket_type);
    (void) zmq_getsockopt (s_, ZMQ_TYPE, &socket_type, &socket_type_size);

    zlink_msg_t frames[zlink_std_compat::k_recv_frame_cap];
    size_t frame_count = 0;
    while (true) {
        if (frame_count >= zlink_std_compat::k_recv_frame_cap) {
            zlink_std_compat::close_frame_range (frames, frame_count);
            errno = EMSGSIZE;
            return -1;
        }

        if (zmq_msg_init (&frames[frame_count]) != 0) {
            zlink_std_compat::close_frame_range (frames, frame_count);
            return -1;
        }

        const int rc = zmq_msg_recv (&frames[frame_count], s_, static_cast<int> (flags_));
        if (rc < 0) {
            zmq_msg_close (&frames[frame_count]);
            zlink_std_compat::close_frame_range (frames, frame_count);
            return -1;
        }

        const int more = zmq_msg_more (&frames[frame_count]);
        ++frame_count;
        if (!more)
            break;
        flags_ = static_cast<zlink_send_flags_t> (flags_ | ZMQ_DONTWAIT);
    }

    size_t start_index = 0;
    if (frame_count > 0 && (socket_type == ZMQ_ROUTER || socket_type == ZMQ_STREAM)) {
        if (source_rid_out_) {
            const size_t rid_size = std::min (static_cast<size_t> (255), zmq_msg_size (&frames[0]));
            source_rid_out_->size = static_cast<uint8_t> (rid_size);
            if (rid_size > 0) {
                std::memcpy (source_rid_out_->data, zmq_msg_data (&frames[0]), rid_size);
            }
        }
        zlink_std_compat::recv_tls_source_rid () = *source_rid_out_;
        start_index = 1;
    }

    const size_t payload_count = frame_count - start_index;
    for (size_t i = 0; i < start_index; ++i)
        zmq_msg_close (&frames[i]);
    for (size_t i = 0; i < payload_count; ++i) {
        if (zlink_std_compat::recv_tls_push (&frames[start_index + i]) != 0) {
            const int saved_errno = errno;
            for (size_t j = i; j < payload_count; ++j)
                zmq_msg_close (&frames[start_index + j]);
            zlink_std_compat::recv_tls_reset ();
            errno = saved_errno;
            return -1;
        }
    }

    *parts_out_ = payload_count > 0 ? &zlink_std_compat::recv_tls_parts ()[0] : NULL;
    *part_count_out_ = zlink_std_compat::recv_tls_count ();
    return 0;
}

inline int zlink_std_compat_router_recv (void *router_,
                                         const zlink_routing_id_t **peer_rid_out_,
                                         uint64_t *request_seq_out_,
                                         zlink_msg_t **parts_out_,
                                         size_t *part_count_out_,
                                         zlink_recv_flags_t flags_)
{
    static thread_local zlink_routing_id_t tls_peer_rid;
    if (request_seq_out_)
        *request_seq_out_ = 0;

    const int rc = zlink_std_compat_recv (router_, &tls_peer_rid, parts_out_, part_count_out_,
                                          static_cast<zlink_send_flags_t> (flags_));
    if (rc != 0) {
        if (peer_rid_out_)
            *peer_rid_out_ = NULL;
        return rc;
    }

    if (peer_rid_out_)
        *peer_rid_out_ = tls_peer_rid.size > 0 ? &tls_peer_rid : NULL;
    zlink_std_compat::recv_tls_source_rid () = tls_peer_rid;
    zlink_std_compat::recv_tls_request_seq () = request_seq_out_ ? *request_seq_out_ : 0;
    return 0;
}

inline int zlink_recv_part (void *s_,
                            const zlink_routing_id_t **source_rid_out_,
                            zlink_msg_t *part_out_,
                            zlink_part_flag_t *has_more_out_,
                            zlink_recv_flags_t flags_)
{
    if (!part_out_ || !has_more_out_) {
        errno = EINVAL;
        return -1;
    }

    if (zlink_std_compat::recv_tls_count () == 0) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        zlink_routing_id_t source_rid;
        std::memset (&source_rid, 0, sizeof (source_rid));
        if (zlink_std_compat_recv (s_, &source_rid, &parts, &part_count, flags_) != 0) {
            if (source_rid_out_)
                *source_rid_out_ = NULL;
            return -1;
        }
    }

    if (source_rid_out_) {
        *source_rid_out_ = zlink_std_compat::recv_tls_source_rid ().size > 0
                             ? &zlink_std_compat::recv_tls_source_rid ()
                             : NULL;
    }
    return zlink_std_compat::recv_tls_take_part (part_out_, has_more_out_);
}

inline int zlink_router_recv_part (void *router_,
                                   const zlink_routing_id_t **peer_rid_out_,
                                   uint64_t *request_seq_out_,
                                   zlink_msg_t *part_out_,
                                   zlink_part_flag_t *has_more_out_,
                                   zlink_recv_flags_t flags_)
{
    if (!part_out_ || !has_more_out_) {
        errno = EINVAL;
        return -1;
    }

    if (zlink_std_compat::recv_tls_count () == 0) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        if (zlink_std_compat_router_recv (router_, peer_rid_out_, request_seq_out_, &parts,
                                          &part_count, flags_)
            != 0) {
            if (peer_rid_out_)
                *peer_rid_out_ = NULL;
            if (request_seq_out_)
                *request_seq_out_ = 0;
            return -1;
        }
    }

    if (peer_rid_out_) {
        *peer_rid_out_ = zlink_std_compat::recv_tls_source_rid ().size > 0
                           ? &zlink_std_compat::recv_tls_source_rid ()
                           : NULL;
    }
    if (request_seq_out_)
        *request_seq_out_ = zlink_std_compat::recv_tls_request_seq ();
    return zlink_std_compat::recv_tls_take_part (part_out_, has_more_out_);
}

inline int zlink_std_compat_publish (void *subject_,
                                     const char *topic_id_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     zlink_send_flags_t flags_)
{
    zlink_msg_t topic_msg;
    const size_t topic_size = topic_id_ ? std::strlen (topic_id_) : 0;
    if (zmq_msg_init_size (&topic_msg, topic_size) != 0)
        return -1;
    if (topic_size > 0)
        std::memcpy (zmq_msg_data (&topic_msg), topic_id_, topic_size);
    if (zmq_msg_send (&topic_msg, subject_, static_cast<int> (flags_ | ZMQ_SNDMORE)) < 0) {
        zmq_msg_close (&topic_msg);
        return -1;
    }
    return zlink_std_compat_send (subject_, parts_, part_count_, flags_);
}

inline int zlink_std_compat_subscribe (void *subject_,
                                       zlink_routing_id_t *source_rid_out_,
                                       zlink_msg_t **parts_out_,
                                       size_t *part_count_out_,
                                       char *topic_id_out_,
                                       size_t *topic_id_len_out_,
                                       zlink_send_flags_t flags_)
{
    if (!parts_out_ || !part_count_out_) {
        errno = EINVAL;
        return -1;
    }

    zlink_std_compat::recv_tls_reset ();
    *parts_out_ = NULL;
    *part_count_out_ = 0;
    if (source_rid_out_)
        source_rid_out_->size = 0;

    zlink_msg_t topic_frame;
    if (zmq_msg_init (&topic_frame) != 0)
        return -1;

    const int rc = zmq_msg_recv (&topic_frame, subject_, static_cast<int> (flags_));
    if (rc < 0) {
        zmq_msg_close (&topic_frame);
        return -1;
    }

    const int topic_more = zmq_msg_more (&topic_frame);
    const size_t topic_len = zmq_msg_size (&topic_frame);
    if (topic_id_len_out_)
        *topic_id_len_out_ = topic_len;
    if (topic_id_out_ && topic_len > 0)
        std::memcpy (topic_id_out_, zmq_msg_data (&topic_frame), topic_len);

    if (!topic_more) {
        zmq_msg_close (&topic_frame);
        return 0;
    }

    size_t payload_count = 0;
    flags_ = static_cast<zlink_send_flags_t> (flags_ | ZMQ_DONTWAIT);
    while (true) {
        zlink_msg_t payload_frame;
        if (zmq_msg_init (&payload_frame) != 0) {
            zmq_msg_close (&topic_frame);
            zlink_std_compat::recv_tls_reset ();
            return -1;
        }

        const int payload_rc = zmq_msg_recv (&payload_frame, subject_, static_cast<int> (flags_));
        if (payload_rc < 0) {
            const int saved_errno = errno;
            zmq_msg_close (&payload_frame);
            zmq_msg_close (&topic_frame);
            zlink_std_compat::recv_tls_reset ();
            errno = saved_errno;
            return -1;
        }

        const int more = zmq_msg_more (&payload_frame);
        if (zlink_std_compat::recv_tls_push (&payload_frame) != 0) {
            const int saved_errno = errno;
            zmq_msg_close (&payload_frame);
            zmq_msg_close (&topic_frame);
            zlink_std_compat::recv_tls_reset ();
            errno = saved_errno;
            return -1;
        }
        ++payload_count;
        if (!more)
            break;
    }

    zmq_msg_close (&topic_frame);

    *parts_out_ = payload_count > 0 ? &zlink_std_compat::recv_tls_parts ()[0] : NULL;
    *part_count_out_ = zlink_std_compat::recv_tls_count ();
    return 0;
}

inline int
zlink_set_tls_server (void *socket_, const char *cert_, const char *key_, int trust_system_)
{
    (void) socket_;
    (void) cert_;
    (void) key_;
    (void) trust_system_;
    return 0;
}

inline int
zlink_set_tls_client (void *socket_, const char *ca_, const char *hostname_, int trust_system_)
{
    (void) socket_;
    (void) ca_;
    (void) hostname_;
    (void) trust_system_;
    return 0;
}

inline uint64_t zlink_translate_monitor_event (int socket_type_, uint16_t raw_event_)
{
    switch (raw_event_) {
        case ZMQ_EVENT_CONNECTED:
#ifdef ZMQ_EVENT_HANDSHAKE_SUCCEEDED
        case ZMQ_EVENT_HANDSHAKE_SUCCEEDED:
#endif
#ifdef ZMQ_EVENT_ACCEPTED
        case ZMQ_EVENT_ACCEPTED:
#endif
            if (socket_type_ == ZMQ_SUB)
                return ZLINK_EVENT_SUB_DELIVERY_READY_CHANGED;
            if (socket_type_ == ZMQ_PUB)
                return ZLINK_EVENT_PUB_DELIVERY_READY_CHANGED;
            return ZLINK_EVENT_CONNECTION_READY;
        case ZMQ_EVENT_BIND_FAILED:
            return ZLINK_EVENT_BIND_FAILED;
#ifdef ZMQ_EVENT_ACCEPT_FAILED
        case ZMQ_EVENT_ACCEPT_FAILED:
            return ZLINK_EVENT_ACCEPT_FAILED;
#endif
#ifdef ZMQ_EVENT_CLOSE_FAILED
        case ZMQ_EVENT_CLOSE_FAILED:
            return ZLINK_EVENT_CLOSE_FAILED;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL
        case ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            return ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL
        case ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            return ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_AUTH
        case ZMQ_EVENT_HANDSHAKE_FAILED_AUTH:
            return ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
#endif
        default:
            return 0;
    }
}

inline int zlink_recv_monitor_event_raw (zlink_monitor_handle_t *handle_,
                                         zlink_socket_monitor_event_t *out_)
{
    if (!handle_ || !handle_->monitor || !out_) {
        errno = EINVAL;
        return -1;
    }

    zmq_msg_t part1;
    zmq_msg_t part2;
    zmq_msg_init (&part1);
    zmq_msg_init (&part2);

    const int n = zmq_msg_recv (&part1, handle_->monitor, ZMQ_DONTWAIT);
    if (n < 0) {
        zmq_msg_close (&part1);
        zmq_msg_close (&part2);
        return -1;
    }
    if (zmq_msg_recv (&part2, handle_->monitor, ZMQ_DONTWAIT) < 0) {
        zmq_msg_close (&part1);
        zmq_msg_close (&part2);
        return -1;
    }

    uint16_t raw_event = 0;
    int32_t raw_value = 0;
    if (zmq_msg_size (&part1) >= sizeof (uint16_t))
        std::memcpy (&raw_event, zmq_msg_data (&part1), sizeof (uint16_t));
    if (zmq_msg_size (&part1) >= sizeof (uint16_t) + sizeof (int32_t)) {
        std::memcpy (&raw_value,
                     static_cast<const char *> (zmq_msg_data (&part1)) + sizeof (uint16_t),
                     sizeof (int32_t));
    }

    std::memset (out_, 0, sizeof (*out_));
    out_->event = zlink_translate_monitor_event (handle_->socket_type, raw_event);
    if (out_->event == ZLINK_EVENT_CONNECTION_READY
        || out_->event == ZLINK_EVENT_SUB_DELIVERY_READY_CHANGED
        || out_->event == ZLINK_EVENT_PUB_DELIVERY_READY_CHANGED) {
        out_->value = handle_->ready_count.fetch_add (1, std::memory_order_acq_rel) + 1;
    } else {
        out_->value = raw_value > 0 ? static_cast<uint64_t> (raw_value) : 0;
    }

    const size_t remote_size = std::min (static_cast<size_t> (255), zmq_msg_size (&part2));
    if (remote_size > 0) {
        std::memcpy (out_->remote_addr, zmq_msg_data (&part2), remote_size);
        out_->remote_addr[remote_size] = '\0';
    }

    zmq_msg_close (&part1);
    zmq_msg_close (&part2);
    return out_->event != 0 ? 0 : -1;
}

inline void zlink_monitor_thread_main (zlink_monitor_handle_t *handle_)
{
    while (handle_ && !handle_->stop.load (std::memory_order_acquire)) {
        zlink_socket_monitor_event_t event;
        if (zlink_recv_monitor_event_raw (handle_, &event) == 0) {
            if (handle_->callback)
                handle_->callback (&event, handle_->userdata);
            continue;
        }
        const int err = zmq_errno ();
        if (err == EAGAIN || err == EINTR) {
            zmq_pollitem_t item = {handle_->monitor, 0, ZMQ_POLLIN, 0};
            (void) zmq_poll (&item, 1, 5);
            continue;
        }
        break;
    }
}

inline void *zlink_socket_monitor_open (void *s_,
                                        const zlink_socket_monitor_open_options_t *options_)
{
    (void) options_;
    void *ctx = g_zlink_last_ctx.load (std::memory_order_acquire);
    if (!ctx) {
        errno = EINVAL;
        return NULL;
    }

    int socket_type = 0;
    size_t socket_type_size = sizeof (socket_type);
    (void) zmq_getsockopt (s_, ZMQ_TYPE, &socket_type, &socket_type_size);

    char endpoint[128];
    const unsigned long seq = g_zlink_monitor_seq.fetch_add (1, std::memory_order_relaxed);
    std::snprintf (endpoint, sizeof (endpoint), "inproc://zlink-compat-mon-%lu", seq);

    int raw_events = ZMQ_EVENT_CONNECTED | ZMQ_EVENT_BIND_FAILED;
#ifdef ZMQ_EVENT_ACCEPTED
    raw_events |= ZMQ_EVENT_ACCEPTED;
#endif
#ifdef ZMQ_EVENT_ACCEPT_FAILED
    raw_events |= ZMQ_EVENT_ACCEPT_FAILED;
#endif
#ifdef ZMQ_EVENT_CLOSE_FAILED
    raw_events |= ZMQ_EVENT_CLOSE_FAILED;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_SUCCEEDED
    raw_events |= ZMQ_EVENT_HANDSHAKE_SUCCEEDED;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL
    raw_events |= ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL
    raw_events |= ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL;
#endif
#ifdef ZMQ_EVENT_HANDSHAKE_FAILED_AUTH
    raw_events |= ZMQ_EVENT_HANDSHAKE_FAILED_AUTH;
#endif

    if (zmq_socket_monitor (s_, endpoint, raw_events) != 0)
        return NULL;

    void *monitor = zmq_socket (ctx, ZMQ_PAIR);
    if (!monitor) {
        zmq_socket_monitor (s_, NULL, 0);
        return NULL;
    }
    if (zmq_connect (monitor, endpoint) != 0) {
        zmq_close (monitor);
        zmq_socket_monitor (s_, NULL, 0);
        return NULL;
    }

    zlink_monitor_handle_t *handle = new zlink_monitor_handle_t ();
    handle->owner = s_;
    handle->monitor = monitor;
    handle->socket_type = socket_type;
    {
        std::lock_guard<std::mutex> lock (g_zlink_monitor_registry_sync);
        g_zlink_monitor_registry.insert (handle);
    }
    return handle;
}

inline int zlink_socket_monitor_handler (void *monitor_,
                                         zlink_socket_monitor_handler_fn handler_,
                                         void *userdata_)
{
    zlink_monitor_handle_t *handle = static_cast<zlink_monitor_handle_t *> (monitor_);
    if (!handle) {
        errno = EINVAL;
        return -1;
    }
    handle->callback = handler_;
    handle->userdata = userdata_;
    if (handler_)
        handle->worker = std::thread (&zlink_monitor_thread_main, handle);
    return 0;
}

inline int zlink_socket_monitor_recv (void *monitor_,
                                      zlink_socket_monitor_event_t *out_,
                                      zlink_send_flags_t flags_)
{
    (void) flags_;
    return zlink_recv_monitor_event_raw (static_cast<zlink_monitor_handle_t *> (monitor_), out_);
}

inline int zlink_monitor_status (void *monitor_, zlink_monitor_status_t *out_)
{
    zlink_monitor_handle_t *handle = static_cast<zlink_monitor_handle_t *> (monitor_);
    if (!handle || !out_) {
        errno = EINVAL;
        return -1;
    }
    std::memset (out_, 0, sizeof (*out_));
    out_->ready_count = handle->ready_count.load (std::memory_order_acquire);
    return 0;
}

inline int zlink_monitor_close (void **monitor_p_)
{
    if (!monitor_p_ || !*monitor_p_)
        return 0;
    zlink_monitor_handle_t *handle = static_cast<zlink_monitor_handle_t *> (*monitor_p_);
    *monitor_p_ = NULL;
    {
        std::lock_guard<std::mutex> lock (g_zlink_monitor_registry_sync);
        g_zlink_monitor_registry.erase (handle);
    }
    handle->stop.store (true, std::memory_order_release);
    if (handle->worker.joinable ())
        handle->worker.join ();
    if (handle->owner)
        zmq_socket_monitor (handle->owner, NULL, 0);
    if (handle->monitor)
        zmq_close (handle->monitor);
    delete handle;
    return 0;
}

inline void *zlink_poller_new (void)
{
    return new zlink_poller_handle_t ();
}

inline int zlink_poller_destroy (void **poller_p_)
{
    if (!poller_p_ || !*poller_p_)
        return 0;
    delete static_cast<zlink_poller_handle_t *> (*poller_p_);
    *poller_p_ = NULL;
    return 0;
}

inline int zlink_poller_size (void *poller_)
{
    if (!poller_)
        return 0;
    return static_cast<int> (static_cast<zlink_poller_handle_t *> (poller_)->items.size ());
}

inline int zlink_poller_add (void *poller_, void *socket_, void *user_data_, short events_)
{
    zlink_poller_handle_t *poller = static_cast<zlink_poller_handle_t *> (poller_);
    if (!poller)
        return -1;
    zlink_poller_item_t item;
    item.socket = socket_;
    item.user_data = user_data_;
    item.events = events_;
    poller->items.push_back (item);
    return 0;
}

inline int zlink_poller_modify (void *poller_, void *socket_, short events_)
{
    zlink_poller_handle_t *poller = static_cast<zlink_poller_handle_t *> (poller_);
    if (!poller)
        return -1;
    for (size_t i = 0; i < poller->items.size (); ++i) {
        if (poller->items[i].socket == socket_) {
            poller->items[i].events = events_;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

inline int zlink_poller_remove (void *poller_, void *socket_)
{
    zlink_poller_handle_t *poller = static_cast<zlink_poller_handle_t *> (poller_);
    if (!poller)
        return -1;
    for (std::vector<zlink_poller_item_t>::iterator it = poller->items.begin ();
         it != poller->items.end (); ++it) {
        if (it->socket == socket_) {
            poller->items.erase (it);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

inline int zlink_poller_wait (void *poller_,
                              zlink_poller_event_t *events_,
                              int n_events_,
                              long timeout_,
                              zlink_config_result_t *error_out_ = NULL)
{
    (void) error_out_;
    zlink_poller_handle_t *poller = static_cast<zlink_poller_handle_t *> (poller_);
    if (!poller)
        return -1;
    if (poller->items.empty ())
        return 0;

    std::vector<zlink_pollitem_t> items (poller->items.size ());
    for (size_t i = 0; i < poller->items.size (); ++i) {
        items[i].socket = poller->items[i].socket;
        items[i].fd = 0;
        items[i].events = poller->items[i].events;
        items[i].revents = 0;
    }
    const int rc = zlink_poll (&items[0], static_cast<int> (items.size ()), timeout_);
    if (rc <= 0)
        return rc;

    int out = 0;
    for (size_t i = 0; i < items.size () && out < n_events_; ++i) {
        if (items[i].revents == 0)
            continue;
        events_[out].socket = poller->items[i].socket;
        events_[out].fd = 0;
        events_[out].user_data = poller->items[i].user_data;
        events_[out].events = items[i].revents;
        ++out;
    }
    return out;
}

inline int zlink_recv_handler (void *socket_, zlink_socket_msg_handler_fn handler_, void *userdata_)
{
    (void) socket_;
    (void) handler_;
    (void) userdata_;
    return 0;
}

inline int
zlink_send_ready_handler (void *socket_, zlink_send_ready_handler_fn handler_, void *userdata_)
{
    (void) socket_;
    (void) handler_;
    (void) userdata_;
    return 0;
}

#endif
