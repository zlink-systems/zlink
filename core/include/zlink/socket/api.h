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
#define ZLINK_AUTO_HWM_MESSAGE_UNIT_BYTES_DFLT ((uint64_t) 4096)
#define ZLINK_AUTO_HWM_STREAM_UNIT_BYTES_DFLT ((uint64_t) 1024)

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

typedef void (*zlink_send_ready_handler_fn) (void *subject_, void *userdata_);

typedef void (*zlink_reply_handler_fn) (zlink_request_result_t result_,
                                        zlink_msg_t *parts_,
                                        size_t part_count_,
                                        void *userdata_);

/**
 * @brief Callback for a bounded raw control record received on a ROUTER's
 * paired completion connection.
 *
 * The callback runs on the socket completion owner. Ownership of every payload
 * part is transferred to the callback; each part must be closed or consumed
 * exactly once before the callback returns. The source routing id is valid only
 * for the duration of the callback.
 */
typedef void (*zlink_completion_control_handler_fn) (
  const zlink_routing_id_t *source_rid_,
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
 * @brief Install or replace the send-ready callback for a send-capable subject.
 *
 * The handler is replace-only. Passing NULL is invalid. A successful replace is
 * visible from the next writable transition. If called reentrantly from the
 * same handle's send-ready callback, the call fails with errno=EDEADLK.
 *
 * Supported handles:
 * - raw `PAIR`
 * - raw `PUB`
 * - raw `XPUB`
 * - raw `DEALER`
 * - raw `ROUTER`
 * - raw `STREAM`
 *
 * Send-ready is independent from receive callback mode. `ZLINK_POLLOUT`
 * observes the same send-recovery readiness axis and may be registered on the
 * same subject. A readiness signal only means it is worth retrying send, not
 * that the retry is guaranteed to succeed.
 *
 * Unsupported subjects fail with errno=ENOTSUP.
 */
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (void *s_,
                                                              zlink_send_ready_handler_fn handler_,
                                                              void *userdata_);

/**
 * @brief Install or replace the ROUTER completion-control callback.
 *
 * Completion-control records are opaque multipart payloads. Core does not
 * assign command meaning or inspect the payload. They are not returned by the
 * application receive APIs. A later registration replaces the current
 * handler. Passing NULL returns ZLINK_HANDLER_INVALID_ARGUMENT with EINVAL;
 * a non-ROUTER socket returns ZLINK_HANDLER_NOT_SUPPORTED with ENOTSUP. Closing
 * the socket while this callback is running returns ZLINK_CLOSE_BUSY with
 * EBUSY; close may be retried after the callback returns.
 */
ZLINK_EXPORT zlink_handler_result_t zlink_router_completion_control_handler (
  void *router_, zlink_completion_control_handler_fn handler_, void *userdata_);

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
 * `ZLINK_OPT_SNDHWM`, `ZLINK_OPT_RCVHWM`, and
 * `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES` require an exact `uint64_t` value.
 * HWM values are bytes and `0` means unlimited. The Auto HWM message unit is
 * a planning input and `0` selects the socket-type default. Four-byte legacy
 * values fail with `ZLINK_CONFIG_INVALID_ARGUMENT`.
 */
ZLINK_EXPORT zlink_config_result_t zlink_set_option (void *handle_,
                                                     zlink_option_t option_,
                                                     const void *optval_,
                                                     size_t optvallen_);

/**
 * @brief Get a common socket option.
 *
 * The three byte-count options documented by zlink_set_option() require an
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
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (void *s_,
                                                    zlink_msg_t *part_,
                                                    zlink_send_flags_t flags_,
                                                    zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (void *s_,
                                                        const zlink_routing_id_t *target_rid_,
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

ZLINK_EXPORT zlink_submit_result_t zlink_router_reply_part (void *router_,
                                                            const zlink_routing_id_t *peer_rid_,
                                                            uint64_t request_seq_,
                                                            zlink_msg_t *part_,
                                                            zlink_part_flag_t part_flag_);

/**
 * @brief Submit one opaque completion-control part to a ROUTER peer.
 *
 * The record uses the peer's existing completion connection. A failed final
 * submit aborts the multipart record. Every call consumes the supplied part on
 * every result. Keep independent copies and retry the complete record from its
 * first part after send-ready notification when the result is
 * ZLINK_SUBMIT_BACKPRESSURED.
 */
ZLINK_EXPORT zlink_submit_result_t zlink_router_completion_control_part (
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);

ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part (void *router_,
                        const zlink_routing_id_t **source_node_rid_out_,
                        uint64_t *request_seq_out_,
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
