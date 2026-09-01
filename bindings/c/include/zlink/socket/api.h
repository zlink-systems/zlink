/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_SOCKET_API_H_INCLUDED
#define ZLINK_SOCKET_API_H_INCLUDED

#include <zlink/common.h>
#include <zlink/message/api.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/*  Raw socket definition.                                                    */
/******************************************************************************/
#define ZLINK_DONTWAIT ZLINK_SEND_FLAGS_DONTWAIT

#define ZLINK_NULL 0
#define ZLINK_PLAIN 1

/* HWM option values are uint64_t byte counts. */
#define ZLINK_HWM_BYTES_DFLT ((uint64_t) 4096000)

/******************************************************************************/
/*  Raw socket events and monitoring                                          */
/******************************************************************************/

#define ZLINK_DISCONNECT_UNKNOWN ZLINK_DISCONNECT_REASON_UNKNOWN
#define ZLINK_DISCONNECT_HANDSHAKE_FAILED ZLINK_DISCONNECT_REASON_HANDSHAKE_FAILED
#define ZLINK_DISCONNECT_TRANSPORT_ERROR ZLINK_DISCONNECT_REASON_TRANSPORT_ERROR
#define ZLINK_DISCONNECT_CTX_TERM ZLINK_DISCONNECT_REASON_CTX_TERM

typedef enum zlink_send_complete_result_t
{
    ZLINK_SEND_ADMITTED = 0,
    ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;

typedef uint64_t zlink_completion_id_t;
/* Opaque socket-local capability. Applications only test zero/nonzero and
 * pass the value back to zlink_reply_part(). */
typedef uint64_t zlink_reply_token_t;

typedef enum zlink_completion_kind_t
{
    ZLINK_COMPLETION_SEND = 1,
    ZLINK_COMPLETION_REQUEST = 2
} zlink_completion_kind_t;

typedef struct zlink_completion_t
{
    /* Caller sets this to sizeof(zlink_completion_t) before receive. */
    uint32_t struct_size;
    zlink_completion_kind_t kind;
    zlink_completion_id_t completion_id;
    void *user_context;
    zlink_routing_id_t peer_rid;
    zlink_send_complete_result_t send_result;
    int send_terminal_errno;
    zlink_request_result_t request_result;
    /* Core-owned contiguous REQUEST reply array; release with
     * zlink_completion_close(), never free directly. */
    zlink_msg_t *reply_parts;
    size_t reply_part_count;
} zlink_completion_t;

typedef enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,
    ZLINK_PART_MORE = 1
} zlink_part_flag_t;

/**
 * @brief Create a socket.
 * @param context_  Context handle (return value of zlink_ctx_new()).
 * @param type_     Socket type.
 * @return Socket handle, or NULL on failure (errno is set).
 */
ZLINK_EXPORT void *zlink_socket (void *, zlink_socket_type_t type_);

/**
 * @brief Close a socket and release its resources.
 *
 * Public handles use a tiered concurrency contract: send/publish hot paths
 * allow same-handle concurrent use, low-frequency control paths serialize for
 * correctness, and close/destroy uses a stricter lifecycle gate. If another
 * thread has an admitted API on the same handle, close fails with errno=EBUSY.
 * Once close is accepted, new API entry fails with errno=ESHUTDOWN.
 */
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);

/**
 * @brief Set a common socket option.
 *
 * `ZLINK_OPT_SNDHWM` and `ZLINK_OPT_RCVHWM` require an exact `uint64_t`
 * value. HWM values are bytes and `0` means unlimited. Four-byte legacy
 * values fail with `ZLINK_CONFIG_INVALID_ARGUMENT`.
 */
ZLINK_EXPORT zlink_config_result_t zlink_set_option (void *handle_,
                                                     zlink_option_t option_,
                                                     const void *optval_,
                                                     size_t optvallen_);

/**
 * @brief Get a common socket option.
 *
 * The two byte-count options documented by zlink_set_option() require an
 * exact `uint64_t` output buffer and return a size of `sizeof(uint64_t)`.
 */
ZLINK_EXPORT zlink_config_result_t zlink_get_option (void *handle_,
                                                     zlink_option_t option_,
                                                     void *optval_,
                                                     size_t *optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_set_routing_id (void *handle_,
                                                         const void *data_,
                                                         size_t size_);
ZLINK_EXPORT zlink_config_result_t zlink_get_routing_id (void *handle_, zlink_routing_id_t *out_);
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_server (void *handle_,
                                                         const char *cert_,
                                                         const char *key_,
                                                         int require_client_cert_);
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_client (void *handle_,
                                                         const char *ca_cert_,
                                                         const char *hostname_,
                                                         int trust_system_);
ZLINK_EXPORT zlink_config_result_t zlink_set_router_option (void *handle_,
                                                            zlink_router_option_t option_,
                                                            const void *optval_,
                                                            size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_router_option (void *handle_,
                                                            zlink_router_option_t option_,
                                                            void *optval_,
                                                            size_t *optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_set_dealer_option (void *handle_,
                                                            zlink_dealer_option_t option_,
                                                            const void *optval_,
                                                            size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_dealer_option (void *handle_,
                                                            zlink_dealer_option_t option_,
                                                            void *optval_,
                                                            size_t *optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_set_stream_option (void *handle_,
                                                            zlink_stream_option_t option_,
                                                            const void *optval_,
                                                            size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_stream_option (void *handle_,
                                                            zlink_stream_option_t option_,
                                                            void *optval_,
                                                            size_t *optvallen_);

/*
 * PUB/XPUB socket:
 * - zlink_pub_option_t for pub-specific options
 * - use zlink_set_option()/zlink_get_option() for common options
 */
ZLINK_EXPORT zlink_config_result_t zlink_set_pub_option (void *handle_,
                                                         zlink_pub_option_t option_,
                                                         const void *optval_,
                                                         size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_pub_option (void *handle_,
                                                         zlink_pub_option_t option_,
                                                         void *optval_,
                                                         size_t *optvallen_);

/*
 * SUB/XSUB socket:
 * - zlink_sub_option_t for sub-specific options
 * - use zlink_set_option()/zlink_get_option() for common options
 */
ZLINK_EXPORT zlink_config_result_t zlink_set_sub_option (void *handle_,
                                                         zlink_sub_option_t option_,
                                                         const void *optval_,
                                                         size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_sub_option (void *handle_,
                                                         zlink_sub_option_t option_,
                                                         void *optval_,
                                                         size_t *optvallen_);

/**
 * @brief Set this socket's local receive-flow state and synchronise it to
 * the paired DEALER/ROUTER completion lane (core-byte-hwm-flow-control-plan.ko.md §5).
 *
 * RUNNING/PAUSED is an absolute state, not a counter: repeating the current
 * state succeeds and resynchronises nothing new. Completion is the point
 * where the socket-owning runtime thread stores the local state; it does not
 * mean the remote peer has already observed it.
 *
 * @return ZLINK_CONFIG_OK on success (including a repeat of the current
 *   state). ZLINK_CONFIG_INVALID_HANDLE for a NULL or invalid handle.
 *   ZLINK_CONFIG_INVALID_ARGUMENT for a state outside
 *   zlink_receive_flow_state_t. ZLINK_CONFIG_NOT_SUPPORTED for a socket type
 *   other than DEALER/ROUTER, which has no completion lane and keeps its
 *   existing byte HWM and transport backpressure unchanged.
 *   ZLINK_CONFIG_INVALID_STATE when a concurrent close is admitted first.
 */
ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);

/**
 * @brief Bind a socket to an address.
 * @param addr_  Endpoint (e.g. @c tcp://host:5555, @c inproc://name).
 */
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);

/** @brief Connect a socket to a remote address. */
ZLINK_EXPORT zlink_connect_result_t zlink_connect (void *s_, const char *addr_);

/** @brief Unbind a socket from an address. */
ZLINK_EXPORT zlink_connect_result_t zlink_unbind (void *s_, const char *addr_);

/** @brief Disconnect a socket from a remote address. */
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_);

/**
 * @brief Disconnect the connected peer whose source routing id matches peer_rid_.
 *
 * Success means the matching pipe was asked to terminate asynchronously.
 */
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_rid (void *s_,
                                                          const zlink_routing_id_t *peer_rid_);

/* ========== Raw part send/receive ========== */
/* Every part call consumes part_ on success and failure. A non-NULL completion
 * output is zeroed before all other validation. user_context_ is non-NULL only
 * for DONTWAIT FINAL; MORE and synchronous NONE FINAL reject it. */
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (void *s_,
                                                    zlink_msg_t *part_,
                                                    zlink_send_flags_t flags_,
                                                    zlink_part_flag_t part_flag_,
                                                    void *user_context_,
                                                    zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (void *s_,
                                                        const zlink_routing_id_t *target_rid_,
                                                        zlink_msg_t *part_,
                                                        zlink_send_flags_t flags_,
                                                        zlink_part_flag_t part_flag_,
                                                        void *user_context_,
                                                        zlink_completion_id_t *completion_id_out_);

/* Request MORE requires timeout_ms_ == 0 and user_context_ == NULL. Successful
 * FINAL reserves a nonzero completion ID; the reply timeout starts only after
 * local outbound admission. Every call consumes part_. */
ZLINK_EXPORT zlink_submit_result_t zlink_request_part (
  void *s_,
  const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);

/* The opaque token comes from every part of one received ROUTER REQUEST.
 * Successful FINAL alone consumes it. Every call consumes part_. */
ZLINK_EXPORT zlink_submit_result_t zlink_reply_part (
  void *router_,
  const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);

/* source_rid_out_ values are socket-owned borrowed views. They remain valid
 * until the same socket's next data-recv entry (success or failure) or close;
 * recv on another socket and completion/monitor/poller operations do not
 * invalidate them. */
ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part (void *router_,
                        const zlink_routing_id_t **source_rid_out_,
                        zlink_reply_token_t *reply_token_out_,
                        zlink_msg_t *part_out_,
                        zlink_part_flag_t *has_more_out_,
                        zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (void *s_,
                                                  const zlink_routing_id_t **source_rid_out_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_,
                                                  zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (void *subject_,
                                                       const char *topic_id_,
                                                       zlink_msg_t *part_,
                                                       zlink_send_flags_t flags_,
                                                       zlink_part_flag_t part_flag_);

/* ========== Raw subscription configuration ========== */
ZLINK_EXPORT zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_);
ZLINK_EXPORT zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_);
ZLINK_EXPORT zlink_config_result_t zlink_subscription_at (
  void *handle_, size_t index_, char *filter_out_, size_t *filter_len_inout_, int *is_pattern_out_);

ZLINK_EXPORT zlink_recv_result_t zlink_subscribe_part (void *sub_,
                                                       const zlink_routing_id_t **source_rid_out_,
                                                       char *topic_id_buf_,
                                                       size_t topic_id_capacity_,
                                                       size_t *topic_id_len_out_,
                                                       zlink_msg_t *part_out_,
                                                       zlink_part_flag_t *has_more_out_,
                                                       zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part (void *xpub_,
                                                       const zlink_routing_id_t **source_rid_out_,
                                                       int *subscribed_out_,
                                                       char *topic_id_buf_,
                                                       size_t topic_id_capacity_,
                                                       size_t *topic_id_len_out_,
                                                       zlink_recv_flags_t flags_);

/* A returned source RID is borrowed from the socket until that same socket's
 * next data-recv entry or close. STREAM receive mode must be set to RAW or
 * PACKET before the first successful bind/connect and is then immutable. */
ZLINK_EXPORT zlink_recv_result_t zlink_stream_recv_packet (
  void *stream_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *header_out_,
  zlink_msg_t *body_out_,
  zlink_recv_flags_t flags_);

/* completion_out_ must be empty: exact struct_size and every other public
 * member zero/empty/NULL. Failures leave an initially empty output empty. */
ZLINK_EXPORT zlink_recv_result_t zlink_completion_recv (
  void *s_, zlink_completion_t *completion_out_, zlink_recv_flags_t flags_);

/* Idempotently closes REQUEST reply storage and resets every field except the
 * preserved struct_size. NULL is a no-op. */
ZLINK_EXPORT void zlink_completion_close (zlink_completion_t *completion_);


#ifdef __cplusplus
}
#endif

#endif
