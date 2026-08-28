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

/**
 * @brief Callback type for direct multipart socket dispatch.
 *
 * Callback is invoked on the owning socket I/O thread.
 * Ownership of all message parts is transferred to the callback.
 * Each part must be closed or otherwise consumed exactly once before return.
 *
 * @param source_rid_ Sender routing id for the received message.
 * @param parts_ Received multipart payload frames.
 * @param part_count_ Number of entries in @p parts_.
 */
typedef void (*zlink_socket_msg_handler_fn) (const zlink_routing_id_t *source_rid_,
                                             zlink_msg_t *parts_,
                                             size_t part_count_,
                                             void *userdata_);

typedef void (*zlink_stream_packet_handler_fn) (void *stream_,
                                                const zlink_routing_id_t *source_rid_,
                                                zlink_msg_t *header_,
                                                zlink_msg_t *body_,
                                                void *userdata_);

/**
 * @brief Value identity of one routed application-pipe submit target.
 *
 * The value is a snapshot, not a reservation. A later exact submit can report
 * backpressure or a terminal route result if the pipe state changed.
 */
typedef struct zlink_routed_submit_target_t
{
    zlink_routing_id_t peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
} zlink_routed_submit_target_t;

/**
 * @brief Outcome of one asynchronous send operation.
 *
 * The enum carries success or final failure only. It never asks the consumer
 * to retry: Core owns the retry, so a completion is always the last word on
 * that operation.
 */
typedef enum zlink_send_complete_result_t
{
    /* Admitted into the Core send queue. Not a peer delivery confirmation. */
    ZLINK_SEND_ADMITTED = 0,
    /* The operation deadline expired before admission. */
    ZLINK_SEND_TIMED_OUT = 201,
    /* Final failure: route termination, cancel, socket close, or context
       termination. `terminal_errno` carries the reason. */
    ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;

/** @brief Core-assigned, socket-local operation id. 0 is never a valid id. */
typedef uint64_t zlink_send_op_id_t;

/**
 * @brief One send completion.
 *
 * `ZLINK_SEND_ADMITTED` means the record entered the Core send queue. It does
 * NOT mean the peer received it. Use request/reply when delivery confirmation
 * is required.
 */
typedef struct zlink_send_complete_event_t
{
    zlink_send_op_id_t op_id;
    void *userdata;
    zlink_routing_id_t peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    zlink_send_complete_result_t result;
    /* Reason when result is not ZLINK_SEND_ADMITTED. ECANCELED for cancel or
       socket close, ETERM for context termination, otherwise the route
       failure errno. 0 when the result is ZLINK_SEND_ADMITTED. */
    int terminal_errno;
} zlink_send_complete_event_t;

/**
 * @brief Send completion callback.
 *
 * Contract:
 * - An operation that returned `ZLINK_SUBMIT_OK` with a non-zero operation id
 *   receives exactly one completion. An operation id of zero means the record
 *   was admitted immediately and no completion callback runs.
 * - Pending admission is FIFO per target. Callback order can differ from
 *   submit order when timeout, cancel, or terminal-route resolution wins.
 * - Completions for one socket never run concurrently with each other.
 * - A callback for a non-zero operation id can run before the corresponding
 *   `zlink_send_async` call returns.
 * - No fixed thread is promised. The callback can run on the Core async
 *   mailbox thread after backpressure clears, on the Core deadline thread on
 *   timeout, on the closing thread during close, or on the thread
 *   that called `zlink_poller_wait` while a `ZLINK_POLLCOMPLETION`
 *   registration owns completion dispatch for this socket.
 * - The callback must only hand the completion to application state. Calling
 *   any send, publish, or request entry point on any socket from inside it
 *   fails with errno=EDEADLK, as does replacing a send-completion handler on
 *   any socket.
 */
typedef void (*zlink_send_complete_handler_fn) (
  void *subject_, const zlink_send_complete_event_t *event_, void *userdata_);

/** @brief Submit options for one asynchronous send. */
typedef struct zlink_send_async_options_t
{
    /* Must be sizeof (zlink_send_async_options_t). */
    uint32_t struct_size;
    /* Per-operation deadline in milliseconds. 0 means no deadline. This is a
       per-operation value and is unrelated to ZLINK_OPT_SNDTIMEO. */
    uint32_t timeout_ms;
    /* Returned unchanged in the completion event. */
    void *userdata;
    /* Routed target, or NULL. ROUTER requires a target. A target with both
       transport-pair fields zero requests an exact-pair snapshot by peer_rid
       during this submit; otherwise all three identity fields select the
       already-snapshotted pair. DEALER may pass NULL, in which case Core
       commits one selection at submit time. PAIR ignores the field. */
    const zlink_routed_submit_target_t *target;
} zlink_send_async_options_t;

typedef void (*zlink_reply_handler_fn) (zlink_request_result_t result_,
                                        zlink_msg_t *parts_,
                                        size_t part_count_,
                                        void *userdata_);

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
 * @brief Attach a direct receive handler to a multipart receive subject.
 *
 * Supported subjects:
 * - raw `STREAM`
 *
 * Unsupported subjects fail with errno=ENOTSUP.
 */
ZLINK_EXPORT zlink_handler_result_t zlink_recv_handler (void *s_,
                                                        zlink_socket_msg_handler_fn handler_,
                                                        void *userdata_);

ZLINK_EXPORT zlink_handler_result_t zlink_stream_packet_handler (
  void *stream_, zlink_stream_packet_handler_fn handler_, void *userdata_);

/**
 * @brief Hand one complete multipart record to Core for asynchronous send.
 *
 * Supported subjects: raw `PAIR`, `DEALER`, `ROUTER`, `STREAM`. Other socket
 * types fail with errno=ENOTSUP.
 *
 * On `ZLINK_SUBMIT_OK` ownership of every entry in `parts_[0 .. part_count_)`
 * transfers to Core and the caller must not touch those messages again, close
 * included. On any other result ownership stays with the caller.
 *
 * The call never blocks. When the target has room the record is admitted on
 * the calling thread, `*op_id_out_` is set to zero, and no completion callback
 * runs. When immediate admission cannot complete, the record is retained,
 * `*op_id_out_` is set to a non-zero id, and exactly one completion reports
 * admission, timeout, cancellation, or termination. That callback may run
 * before this function returns. The byte
 * high-water mark accounts the record as one message, exactly as a synchronous
 * multipart send does.
 *
 * Pending operations wait without a limit by default, preserving asynchronous
 * admission semantics under normal HWM pressure. An application may opt into
 * bounds with `ZLINK_OPT_SEND_PENDING_MAX_MSGS` and
 * `ZLINK_OPT_SEND_PENDING_MAX_BYTES`; zero means unlimited. Exceeding a
 * configured non-zero bound fails with `ZLINK_SUBMIT_BACKPRESSURED` and leaves
 * part ownership with the caller.
 *
 * Pending operations for one target are admitted in submit order. Completion
 * callbacks are serialized per socket but may be reordered by timeout,
 * cancellation, or terminal-route resolution. A blocked target alone does not
 * make another target pending, and a synchronous send competes for the same
 * high-water mark on equal terms.
 * A completion callback must be installed with `zlink_send_complete_handler`
 * first because the call can become pending; without one the call fails with
 * errno=EINVAL. `op_id_out_` may be NULL when the caller does not need to
 * distinguish immediate admission from pending completion.
 *
 * `ZLINK_SEND_ADMITTED` means admission into the Core send queue, not peer
 * delivery.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_async (
  void *s_,
  zlink_msg_t *parts_,
  size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

/**
 * @brief Install or replace the send completion callback.
 *
 * Replace-only; passing NULL is invalid. Supported subjects are the same set
 * `zlink_send_async` accepts. Replacing a send-completion handler on any
 * socket from inside a completion callback fails with errno=EDEADLK.
 *
 * Registering the socket on a poller with `ZLINK_POLLCOMPLETION` transfers
 * dispatch ownership of this callback from the Core async mailbox thread to
 * the thread that calls `zlink_poller_wait`. That is a change of dispatch
 * location only: the same callback, the same event, the same guarantees. The
 * two dispatch owners are mutually exclusive per socket.
 */
ZLINK_EXPORT zlink_handler_result_t zlink_send_complete_handler (
  void *s_, zlink_send_complete_handler_fn handler_, void *userdata_);

/**
 * @brief Request cancellation of one not-yet-completed operation.
 *
 * `ZLINK_SUBMIT_OK` means the cancel was accepted and the completion will
 * report `ZLINK_SEND_TERMINAL` with errno `ECANCELED`.
 * `ZLINK_SUBMIT_NOT_FOUND` means no pending operation carries that id.
 * `ZLINK_SUBMIT_INVALID_STATE` means another resolver already claimed the
 * operation. That resolver still reports exactly one completion, normally
 * `ZLINK_SEND_ADMITTED` but possibly `ZLINK_SEND_TERMINAL` for a route failure
 * already being resolved.
 *
 * A cancelled operation still completes exactly once.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_async_cancel (
  void *s_, zlink_send_op_id_t op_id_);

/**
 * @brief Select one exact routed submit target without claiming pipe credit.
 *
 * ROUTER requires a non-NULL routing id and snapshots that exact admitted
 * route. DEALER requires NULL and commits one weighted selection across all
 * connected, positive-weight application pipes, including an HWM-blocked
 * pipe. The returned value can be inserted into binding-owned pending state
 * before an exact DONTWAIT submit.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_select_routed_submit_target (
  void *s_, const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_);

/**
 * @brief Close a socket and release its resources.
 *
 * Public handles use a tiered concurrency contract: send/publish hot paths
 * allow same-handle concurrent use, low-frequency control paths serialize for
 * correctness, and close/destroy uses a stricter lifecycle gate. If another
 * thread has an in-flight callback or admitted API on the same handle, close
 * fails with errno=EBUSY. Once close is accepted, new API entry fails with
 * errno=ESHUTDOWN. Self-close from a send-ready or monitor callback is
 * deferred until callback epilogue. For STREAM raw callbacks, close from
 * inside the raw callback is not supported and fails with errno=EBUSY.
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

/**
 * @brief Disconnect the exact transport pair identified by its monitor identity.
 *
 * The pair id and generation must be copied from a monitor event for the
 * connection to be terminated.  This operation does not affect another
 * connection that uses the same peer routing id.
 */
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_transport_pair (
  void *s_, uint64_t transport_pair_id_, uint64_t transport_pair_generation_);

/* ========== Raw part send/receive ========== */
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (void *s_,
                                                    zlink_msg_t *part_,
                                                    zlink_send_flags_t flags_,
                                                    zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (void *s_,
                                                        const zlink_routing_id_t *target_rid_,
                                                        zlink_msg_t *part_,
                                                        zlink_send_flags_t flags_,
                                                        zlink_part_flag_t part_flag_);

/**
 * @brief Submit one ROUTER part only through the specified transport pair.
 *
 * The routing id and pair identity must describe the same admitted peer. The
 * operation does not fall back to another physical connection with the same
 * routing id. The pair identity is copied from a monitor event.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_transport_pair (
  void *s_,
  const zlink_routing_id_t *target_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_submit_result_t zlink_dealer_request_part (void *dealer_,
                                                              zlink_msg_t *part_,
                                                              zlink_send_flags_t flags_,
                                                              zlink_part_flag_t part_flag_,
                                                              uint32_t timeout_ms_,
                                                              zlink_reply_handler_fn handler_,
                                                              void *userdata_);

/** @brief Submit a DEALER request part to one selected exact target. */
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_request_transport_pair_part (
  void *dealer_,
  const zlink_routed_submit_target_t *target_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);

/** @brief Submit a DEALER raw part to one selected exact target. */
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_send_transport_pair_part (
  void *dealer_,
  const zlink_routed_submit_target_t *target_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_recv_result_t zlink_dealer_recv_part (void *dealer_,
                                                         uint8_t *message_type_out_,
                                                         uint64_t *request_seq_out_,
                                                         zlink_msg_t *part_out_,
                                                         zlink_part_flag_t *has_more_out_,
                                                         zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_reply_part (void *dealer_,
                                                            uint64_t request_seq_,
                                                            zlink_msg_t *part_,
                                                            zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_submit_result_t zlink_router_request_part (void *router_,
                                                              const zlink_routing_id_t *peer_rid_,
                                                              zlink_msg_t *part_,
                                                              zlink_send_flags_t flags_,
                                                              zlink_part_flag_t part_flag_,
                                                              uint32_t timeout_ms_,
                                                              zlink_reply_handler_fn handler_,
                                                              void *userdata_);

/** @brief Submit a ROUTER request part through one specified transport pair. */
ZLINK_EXPORT zlink_submit_result_t zlink_router_request_transport_pair_part (
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);

ZLINK_EXPORT zlink_submit_result_t zlink_router_reply_part (void *router_,
                                                            const zlink_routing_id_t *peer_rid_,
                                                            uint64_t request_seq_,
                                                            zlink_msg_t *part_,
                                                            zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part (void *router_,
                        const zlink_routing_id_t **source_node_rid_out_,
                        uint64_t *request_seq_out_,
                        zlink_msg_t *part_out_,
                        zlink_part_flag_t *has_more_out_,
                        zlink_recv_flags_t flags_);

/** @brief Receives one Router part and returns the source transport pair identity. */
ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part_v2 (void *router_,
                           const zlink_routing_id_t **source_node_rid_out_,
                           uint64_t *request_seq_out_,
                           uint64_t *transport_pair_id_out_,
                           uint64_t *transport_pair_generation_out_,
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


#ifdef __cplusplus
}
#endif

#endif
