/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_EVENTING_API_H_INCLUDED
#define ZLINK_EVENTING_API_H_INCLUDED

#include <zlink/common.h>
#include <zlink/socket/api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint64_t event;
    uint64_t value;
    zlink_routing_id_t routing_id;
    char local_addr[256];
    char remote_addr[256];
} zlink_monitor_event_t;

typedef void (*zlink_monitor_handler_fn) (const zlink_monitor_event_t *event_, void *userdata_);

typedef zlink_monitor_event_t zlink_socket_monitor_event_t;
typedef zlink_monitor_handler_fn zlink_socket_monitor_handler_fn;

#define ZLINK_MONITOR_STATUS_ABI_VERSION 2u

typedef struct zlink_socket_monitor_open_options_t
{
    zlink_socket_monitor_event_mask_t events;
} zlink_socket_monitor_open_options_t;

/**
 * @brief Ignore socket monitor events while keeping a valid handler symbol.
 *
 * Pass this when you want snapshot or direct polling on the returned monitor
 * handle without automatic callback dispatch.
 */
ZLINK_EXPORT void zlink_monitor_ignore_handler (const zlink_monitor_event_t *event_,
                                                void *userdata_);

typedef struct zlink_monitor_status_t
{
    /* Snapshot ABI version. Equals ZLINK_MONITOR_STATUS_ABI_VERSION. */
    uint32_t abi_version;

    /* Size of this snapshot structure in bytes. */
    uint32_t struct_size;

    /* Snapshot source kind: raw socket. */
    zlink_monitor_source_kind_t source_kind;

    /* Current state bits such as READY, BOUND_READY, and CLOSED. */
    zlink_monitor_state_mask_t state_flags;

    /* Bitmask describing which detail fields are populated. */
    zlink_monitor_status_detail_mask_t detail_flags;

    /* Current pending send message count. */
    uint64_t snd_pending_msgs;

    /* Current pending receive message count. Some sources report an estimate. */
    uint64_t rcv_pending_msgs;

    /* Non-zero when automatic HWM policy is enabled for this source. */
    uint32_t auto_hwm_enabled;

    /* Current automatic HWM profile value. Matches zlink_auto_hwm_profile_t. */
    uint32_t auto_hwm_profile;

    /* Socket role used by the automatic HWM calculation. Diagnostic only. */
    uint32_t auto_hwm_role;

    /* Automatic HWM policy class selected from role and socket type. */
    uint32_t auto_hwm_policy_class;

    /* Unit budget in bytes used to calculate message slots for this socket. */
    uint64_t auto_hwm_unit_budget_bytes;

    /* Message slot cap selected from profile and policy class. */
    uint32_t auto_hwm_size_cap;

    /* Message slot count calculated from the unit budget and message size. */
    uint64_t auto_hwm_socket_message_slots;

    /* Non-zero when a connection-count bucket limited this socket plan. */
    uint32_t auto_hwm_connection_bucket_enabled;

    /* Connection count observed by the automatic HWM bucket planner. */
    uint32_t auto_hwm_connection_bucket_count;

    /* Selected connection bucket index, or UINT32_MAX when no bucket applies. */
    uint32_t auto_hwm_connection_bucket_index;

    /* Selected bucket HWM for a 4 KiB message unit, or 0 when no bucket applies. */
    uint32_t auto_hwm_connection_bucket_hwm_4k;

    /* Non-zero when hysteresis retained the previous connection bucket. */
    uint32_t auto_hwm_connection_bucket_hysteresis_retained;

    /* Message size in bytes used by the automatic HWM calculation. */
    uint64_t auto_hwm_effective_message_bytes;

    /* Send HWM selected by the current plan, in accounted bytes. */
    uint64_t auto_hwm_planned_sndhwm_bytes;

    /* Receive HWM selected by the current plan, in accounted bytes. */
    uint64_t auto_hwm_planned_rcvhwm_bytes;

    /* Current send HWM applied to the socket, in accounted bytes. */
    uint64_t auto_hwm_applied_sndhwm_bytes;

    /* Current receive HWM applied to the socket, in accounted bytes. */
    uint64_t auto_hwm_applied_rcvhwm_bytes;

    /* Current send buffer size applied to the socket, in bytes. */
    int32_t auto_hwm_effective_sndbuf;

    /* Current receive buffer size applied to the socket, in bytes. */
    int32_t auto_hwm_effective_rcvbuf;

    /* Last automatic HWM recalculation timestamp, in milliseconds. */
    uint64_t auto_hwm_last_recalc_ms;

    /* Last automatic HWM recalculation reason. ZLINK_AUTO_HWM_RECALC_REASON_*. */
    uint32_t auto_hwm_last_recalc_reason;

    /* Ratio of send attempts blocked by backpressure, in ppm. */
    uint32_t auto_hwm_send_blocked_ratio_ppm;

    /* Target send HWM while shrink is deferred, in accounted bytes. */
    uint64_t auto_hwm_deferred_sndhwm_bytes;

    /* Target receive HWM while shrink is deferred, in accounted bytes. */
    uint64_t auto_hwm_deferred_rcvhwm_bytes;

    /* Non-zero when auto_hwm_deferred_sndhwm_bytes is valid. */
    uint32_t auto_hwm_deferred_sndhwm_valid;

    /* Non-zero when auto_hwm_deferred_rcvhwm_bytes is valid. */
    uint32_t auto_hwm_deferred_rcvhwm_valid;

    /* Current bytes retained by outbound pipe directions. */
    uint64_t snd_bytes_in_flight;

    /* Current bytes retained by inbound pipe directions. */
    uint64_t rcv_bytes_in_flight;

    /* Minimum accounted charge for one Core frame. */
    uint64_t minimum_core_message_charge_bytes;

    /* Number of messages admitted by the empty-pipe oversize rule. */
    uint64_t oversize_message_admission_count;

    /* Largest accounted message admitted by the empty-pipe oversize rule. */
    uint64_t oversize_message_admission_max_bytes;
} zlink_monitor_status_t;

/**
 * @brief Open and return a socket monitor handle directly.
 * @param events_  Event bitmask.
 * @return Monitor handle, or NULL on failure.
 */
ZLINK_EXPORT void *zlink_socket_monitor_open (void *s_,
                                              const zlink_socket_monitor_open_options_t *options_);

ZLINK_EXPORT zlink_handler_result_t zlink_socket_monitor_handler (
  void *monitor_, zlink_socket_monitor_handler_fn handler_, void *userdata_);

ZLINK_EXPORT zlink_recv_result_t zlink_socket_monitor_recv (void *monitor_,
                                                            zlink_socket_monitor_event_t *out_,
                                                            zlink_recv_flags_t flags_);

/** @brief Read the current snapshot for a monitor handle. */
ZLINK_EXPORT zlink_config_result_t zlink_monitor_status (void *monitor_,
                                                         zlink_monitor_status_t *out_);

ZLINK_EXPORT zlink_close_result_t zlink_monitor_close (void **monitor_p_);


#if defined _WIN32
#if defined _WIN64
typedef unsigned __int64 zlink_fd_t;
#else
typedef unsigned int zlink_fd_t;
#endif
#else
typedef int zlink_fd_t;
#endif

typedef struct zlink_pollitem_t
{
    void *socket;
    zlink_fd_t fd;
    short events;
    short revents;
} zlink_pollitem_t;

typedef struct zlink_poller_event_t
{
    zlink_poller_source_kind_t source_kind;
    void *socket;
    zlink_fd_t fd;
    void *timer;
    void *user_data;
    short events;
} zlink_poller_event_t;

#ifndef ZLINK_HAVE_POLLER
#define ZLINK_HAVE_POLLER 1
#endif

ZLINK_EXPORT int zlink_poll (zlink_pollitem_t *items_,
                             int nitems_,
                             long timeout_,
                             zlink_config_result_t *error_out_);

ZLINK_EXPORT void *zlink_poller_new (void);
ZLINK_EXPORT zlink_close_result_t zlink_poller_destroy (void **poller_p_);
ZLINK_EXPORT int zlink_poller_size (void *poller_, zlink_config_result_t *error_out_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add (void *poller_,
                                                     void *socket_,
                                                     void *user_data_,
                                                     short events_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify (void *poller_,
                                                        void *socket_,
                                                        short events_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove (void *poller_, void *socket_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_fd (void *poller_,
                                                        zlink_fd_t fd_,
                                                        void *user_data_,
                                                        short events_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_timer (void *poller_,
                                                           void *timer_,
                                                           void *user_data_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify_fd (void *poller_,
                                                           zlink_fd_t fd_,
                                                           short events_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_fd (void *poller_, zlink_fd_t fd_);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_timer (void *poller_, void *timer_);
ZLINK_EXPORT int zlink_poller_wait (void *poller_,
                                    zlink_poller_event_t *events_,
                                    int n_events_,
                                    long timeout_,
                                    zlink_config_result_t *error_out_);
/******************************************************************************/
/*  Timers                                                                    */
/******************************************************************************/

typedef void (*zlink_timer_handler_fn) (void *timer_, uint64_t fire_count_, void *userdata_);

ZLINK_EXPORT void *zlink_timer_new (void);
ZLINK_EXPORT zlink_close_result_t zlink_timer_destroy (void **timer_p_);
ZLINK_EXPORT zlink_config_result_t zlink_timer_start (void *timer_,
                                                      uint64_t interval_ns_,
                                                      uint64_t repeat_count_);
ZLINK_EXPORT zlink_config_result_t zlink_timer_stop (void *timer_);
ZLINK_EXPORT zlink_recv_result_t zlink_timer_recv (void *timer_, uint64_t *fire_count_out_);
ZLINK_EXPORT zlink_handler_result_t zlink_timer_handler (void *timer_,
                                                         zlink_timer_handler_fn handler_,
                                                         void *userdata_);

#ifdef __cplusplus
}
#endif

#endif
