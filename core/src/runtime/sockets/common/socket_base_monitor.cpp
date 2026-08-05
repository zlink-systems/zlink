/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/monitor_api_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/ctx.hpp"
#include "core/send_internal.hpp"
#include "core/control_runtime.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/debug_log.hpp"
#include "utils/sleep.hpp"
#include "zlink.h"

namespace
{
uint32_t compute_blocked_ratio_ppm_local (uint64_t attempts_, uint64_t blocked_)
{
    if (attempts_ == 0 || blocked_ == 0)
        return 0u;
    const uint64_t scaled = (blocked_ * 1000000ull) / attempts_;
    return scaled > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t> (scaled);
}

bool add_snapshot_counter (uint64_t *value_, uint64_t delta_)
{
    if (!value_ || UINT64_MAX - *value_ < delta_) {
        errno = EOVERFLOW;
        return false;
    }
    *value_ += delta_;
    return true;
}

}

int zlink::socket_base_t::monitor_snapshot (zlink_monitor_status_t *out_)
{
    if (!out_) {
        errno = EINVAL;
        return -1;
    }

    process_commands (0, false);
    memset (out_, 0, sizeof (*out_));
    out_->abi_version = ZLINK_MONITOR_STATUS_ABI_VERSION;
    out_->struct_size = static_cast<uint32_t> (sizeof (*out_));
    out_->source_kind = ZLINK_MONITOR_SOURCE_SOCKET;
    out_->detail_flags =
      ZLINK_MONITOR_STATUS_DETAIL_SND_PENDING_MSGS | ZLINK_MONITOR_STATUS_DETAIL_RCV_PENDING_MSGS
      | ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET | ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        if (monitor_ready_count () > 0)
            out_->state_flags |= ZLINK_MONITOR_STATE_READY;

        for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i) {
            pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
            if (!add_snapshot_counter (
                  &out_->snd_pending_msgs, pipe->get_snd_pending_msgs ())
                || !add_snapshot_counter (
                  &out_->rcv_pending_msgs, pipe->get_rcv_pending_msgs_approx ())
                || !add_snapshot_counter (
                  &out_->snd_bytes_in_flight, pipe->get_snd_pending_bytes ())
                || !add_snapshot_counter (
                  &out_->rcv_bytes_in_flight, pipe->get_rcv_pending_bytes_approx ())
                || !add_snapshot_counter (
                  &out_->oversize_message_admission_count,
                  pipe->get_oversize_message_admission_count ())) {
                return -1;
            }
            out_->oversize_message_admission_max_bytes =
              std::max (out_->oversize_message_admission_max_bytes,
                        pipe->get_oversize_message_admission_max_bytes ());
        }
    }
    out_->auto_hwm_enabled = _auto_hwm_context_plan.enabled ? 1u : 0u;
    out_->auto_hwm_profile = static_cast<uint32_t> (_auto_hwm_context_plan.profile);
    out_->auto_hwm_role = static_cast<uint32_t> (_auto_hwm_socket_plan.role);
    out_->auto_hwm_policy_class = static_cast<uint32_t> (_auto_hwm_socket_plan.policy_class);
    out_->auto_hwm_unit_budget_bytes = _auto_hwm_socket_plan.unit_budget_bytes;
    out_->auto_hwm_size_cap = _auto_hwm_socket_plan.size_cap;
    out_->auto_hwm_socket_message_slots = _auto_hwm_socket_plan.socket_message_slots;
    out_->auto_hwm_connection_bucket_enabled =
      _auto_hwm_socket_plan.connection_bucket_enabled ? 1u : 0u;
    out_->auto_hwm_connection_bucket_count = _auto_hwm_socket_plan.connection_bucket_count;
    out_->auto_hwm_connection_bucket_index = _auto_hwm_socket_plan.connection_bucket_index;
    out_->auto_hwm_connection_bucket_hwm_4k = _auto_hwm_socket_plan.connection_bucket_hwm_4k;
    out_->auto_hwm_connection_bucket_hysteresis_retained =
      _auto_hwm_socket_plan.connection_bucket_hysteresis_retained ? 1u : 0u;
    out_->auto_hwm_effective_message_bytes = _auto_hwm_socket_plan.effective_message_bytes;
    out_->auto_hwm_planned_sndhwm_bytes = _auto_hwm_socket_plan.sndhwm;
    out_->auto_hwm_planned_rcvhwm_bytes = _auto_hwm_socket_plan.rcvhwm;
    out_->auto_hwm_applied_sndhwm_bytes = options.sndhwm;
    out_->auto_hwm_applied_rcvhwm_bytes = options.rcvhwm;
    out_->auto_hwm_effective_sndbuf = options.sndbuf;
    out_->auto_hwm_effective_rcvbuf = options.rcvbuf;
    out_->auto_hwm_last_recalc_ms = _auto_hwm_last_recalc_ms;
    out_->auto_hwm_last_recalc_reason = _auto_hwm_last_recalc_reason;
    out_->auto_hwm_send_blocked_ratio_ppm = compute_blocked_ratio_ppm_local (
      _auto_hwm_send_attempts.load (std::memory_order_relaxed),
      _auto_hwm_send_blocked_attempts.load (std::memory_order_relaxed));
    out_->auto_hwm_deferred_sndhwm_bytes = _auto_hwm_deferred_sndhwm;
    out_->auto_hwm_deferred_rcvhwm_bytes = _auto_hwm_deferred_rcvhwm;
    out_->auto_hwm_deferred_sndhwm_valid =
      _auto_hwm_deferred_sndhwm_valid ? 1u : 0u;
    out_->auto_hwm_deferred_rcvhwm_valid =
      _auto_hwm_deferred_rcvhwm_valid ? 1u : 0u;
    out_->minimum_core_message_charge_bytes = sizeof (msg_t);

    return 0;
}

uint32_t zlink::socket_base_t::monitor_ready_count () const
{
    return monitor_runtime ().ready_count ();
}

bool zlink::socket_base_t::has_attached_pipes () const
{
    scoped_lock_t lock (monitor_runtime ().sync);
    return endpoint_runtime ().has_attached_pipes ();
}

bool zlink::socket_base_t::monitor_has_attached_pipes () const
{
    return has_attached_pipes ();
}

void zlink::socket_base_t::socket_peer_remote_endpoints (std::vector<std::string> *out_)
{
    if (!out_)
        return;

    process_commands (0, false);
    out_->clear ();
    scoped_lock_t lock (monitor_runtime ().sync);
    out_->reserve (endpoint_runtime ().attached_pipe_count ());
    for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i) {
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        const std::string &remote = pipe->get_endpoint_pair ().remote;
        if (!remote.empty ())
            out_->push_back (remote);
    }
}

void zlink::socket_base_t::snapshot_attached_pipes (std::vector<pipe_t *> *out_)
{
    if (!out_)
        return;

    process_commands (0, false);
    out_->clear ();
    scoped_lock_t lock (monitor_runtime ().sync);
    out_->reserve (endpoint_runtime ().attached_pipe_count ());
    for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i)
        out_->push_back (endpoint_runtime ().attached_pipe (i));
}

int zlink::socket_base_t::monitor (const char *endpoint_,
                                   uint64_t events_,
                                   int event_version_,
                                   int type_)
{
    monitor_runtime_t &monitor = monitor_runtime ();
    scoped_lock_t lock (monitor.sync);

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    if (endpoint_ == NULL) {
        stop_monitor ();
        return 0;
    }

    std::string protocol;
    std::string address;
    if (parse_uri (endpoint_, protocol, address) || check_protocol (protocol))
        return -1;

    if (protocol != protocol_name::inproc) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    if (monitor.socket != NULL)
        stop_monitor (true);

    switch (type_) {
        case ZLINK_CORE_SOCKET_PAIR:
        case ZLINK_CORE_SOCKET_PUB:
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    monitor.events = events_;
    monitor.lossy = event_version_ <= 3;
    lifecycle_coordinator ().set_monitor_async_mailbox_owned (false);
    monitor.socket = static_cast<void *> (get_ctx ()->create_socket (type_));
    if (monitor.socket == NULL)
        return -1;

    int linger = 0;
    int rc = static_cast<socket_base_t *> (monitor.socket)
               ->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &linger, sizeof (linger));
    if (rc == -1)
        stop_monitor (false);

    // The monitor worker retries non-lossy deliveries in user space. Keep the
    // underlying PAIR socket non-blocking so shutdown can stop the worker even
    // if the peer disappears mid-send.
    const int sndtimeo = 0;
    rc = static_cast<socket_base_t *> (monitor.socket)
           ->setsockopt (ZLINK_INTERNAL_OPT_SNDTIMEO, &sndtimeo, sizeof (sndtimeo));
    if (rc == -1)
        stop_monitor (false);

    rc = zlink_bind (monitor.socket, endpoint_);
    if (rc == -1)
        stop_monitor (false);
    else {
        monitor.reset_worker_state ();
        control_runtime_t *runtime = get_ctx ()->control_runtime ();
        const uint64_t task_id =
          runtime ? runtime->add_periodic_task (&socket_base_t::monitor_task_main, this, 10, true)
                  : 0;
        if (task_id == 0) {
            stop_monitor (false);
            return -1;
        }
        monitor.start_task (task_id);
        monitor.events_atomic.store (monitor.events, std::memory_order_release);

    }
    return rc;
}

void zlink::socket_base_t::event_connected (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                            zlink::fd_t fd_)
{
    uint64_t values[1] = {static_cast<uint64_t> (fd_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_CONNECTED);
}

void zlink::socket_base_t::event_connect_delayed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                                  int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_CONNECT_DELAYED);
}

void zlink::socket_base_t::event_connect_retried (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                                  int interval_)
{
    uint64_t values[1] = {static_cast<uint64_t> (interval_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_CONNECT_RETRIED);
}

void zlink::socket_base_t::event_listening (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                            zlink::fd_t fd_)
{
    uint64_t values[1] = {static_cast<uint64_t> (fd_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_LISTENING);
}

void zlink::socket_base_t::event_bind_failed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                              int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_BIND_FAILED);
}

void zlink::socket_base_t::event_accepted (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                           zlink::fd_t fd_)
{
    uint64_t values[1] = {static_cast<uint64_t> (fd_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_ACCEPTED);
}

void zlink::socket_base_t::event_accept_failed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                                int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_ACCEPT_FAILED);
}

void zlink::socket_base_t::event_closed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                         zlink::fd_t fd_)
{
    uint64_t values[1] = {static_cast<uint64_t> (fd_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_CLOSED);
}

void zlink::socket_base_t::event_close_failed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                               int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_CLOSE_FAILED);
}

void zlink::socket_base_t::event_disconnected (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                               uint64_t reason_,
                                               const unsigned char *routing_id_,
                                               size_t routing_id_size_)
{
    if (monitor_runtime ().events_atomic.load (std::memory_order_acquire) == 0)
        return;

    uint32_t ready_count = 0;
    bool changed = false;
    bool enqueue_disconnected = false;
    bool enqueue_ready_count = false;
    monitor_event_record_t disconnected_record;
    monitor_event_record_t ready_count_record;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        monitor_runtime ().erase_transport_pair_readiness_for_endpoint (
          endpoint_uri_pair_);
        if (monitor_runtime ().events & ZLINK_EVENT_DISCONNECTED) {
            uint64_t values[1] = {reason_};
            enqueue_disconnected = build_monitor_event_record (
              &disconnected_record, ZLINK_EVENT_DISCONNECTED, values, 1,
              routing_id_, routing_id_size_, endpoint_uri_pair_);
        }

        changed = monitor_runtime ().erase_ready_connection (endpoint_uri_pair_, routing_id_,
                                                             routing_id_size_, &ready_count);
        if (!changed)
            changed = monitor_runtime ().erase_ready_connection_for_endpoint (endpoint_uri_pair_,
                                                                              &ready_count);

        LIBZLINK_UNUSED (changed);
        if (monitor_runtime ().events & ZLINK_EVENT_CONNECTION_READY) {
            uint64_t ready_values[1] = {ready_count};
            enqueue_ready_count = build_monitor_event_record (
              &ready_count_record, ZLINK_EVENT_CONNECTION_READY, ready_values,
              1, routing_id_, routing_id_size_, endpoint_uri_pair_);
        }
    }
    //  Reliable internal monitors may apply queue backpressure. Never wait for
    //  that capacity while holding monitor.sync, which shutdown also needs.
    if (enqueue_disconnected)
        enqueue_monitor_event (disconnected_record);
    if (enqueue_ready_count)
        enqueue_monitor_event (ready_count_record);
}

void zlink::socket_base_t::event_handshake_failed_no_detail (
  const endpoint_uri_pair_t &endpoint_uri_pair_, int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL);
}

void zlink::socket_base_t::event_handshake_failed_protocol (
  const endpoint_uri_pair_t &endpoint_uri_pair_, int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL);
}

void zlink::socket_base_t::event_handshake_failed_auth (
  const endpoint_uri_pair_t &endpoint_uri_pair_, int err_)
{
    uint64_t values[1] = {static_cast<uint64_t> (err_)};
    event (endpoint_uri_pair_, NULL, 0, values, 1, ZLINK_EVENT_HANDSHAKE_FAILED_AUTH);
}

void zlink::socket_base_t::event_connection_ready_changed (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  const unsigned char *routing_id_,
  size_t routing_id_size_)
{
    uint32_t ready_count = 0;
    bool changed = false;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        changed = monitor_runtime ().mark_ready_connection (endpoint_uri_pair_, routing_id_,
                                                            routing_id_size_, &ready_count);
    }
    if (!changed)
        return;

    uint64_t values[1] = {ready_count};
    event (endpoint_uri_pair_, routing_id_, routing_id_size_, values, 1,
           ZLINK_EVENT_CONNECTION_READY,
           socket_monitor_internal_connection_ready_edge);
}

void zlink::socket_base_t::event_transport_pair_lane_ready (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  transport_lane_t lane_,
  uint64_t pair_id_,
  uint64_t generation_)
{
    if ((monitor_runtime ().events_atomic.load (std::memory_order_acquire)
         & ZLINK_EVENT_CONNECTION_READY)
        == 0)
        return;

    bool pair_ready = false;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        pair_ready = monitor_runtime ().mark_transport_pair_lane_ready (
          endpoint_uri_pair_, lane_, pair_id_, generation_);
    }
    if (pair_ready)
        event_connection_ready_changed (
          endpoint_uri_pair_, routing_id_, routing_id_size_);
}

void zlink::socket_base_t::emit_inproc_connection_ready (pipe_t *pipe_)
{
    if (!pipe_)
        return;

    if (!pipe_->mark_connection_ready_event_emitted ())
        return;

    const endpoint_uri_pair_t &endpoint_pair = pipe_->get_endpoint_pair ();
    const blob_t &routing_id = pipe_->get_routing_id ();
    const unsigned char *routing_id_data = routing_id.size () > 0 ? routing_id.data () : NULL;
    const uint64_t pair_id = pipe_->get_transport_pair_id ();
    if (pair_id != 0) {
        //  inproc has no engine handshake, so the socket reports lane
        //  readiness itself. The pair-aware entry keeps the public contract of
        //  one ready event per Application·Completion pair, exactly as the
        //  engine transports report it.
        event_transport_pair_lane_ready (endpoint_pair, routing_id_data, routing_id.size (),
                                         pipe_->get_transport_lane (), pair_id,
                                         pipe_->get_transport_pair_generation ());
        return;
    }
    event_connection_ready_changed (endpoint_pair, routing_id_data, routing_id.size ());
}

void zlink::socket_base_t::validate_inproc_connection (pipe_t *pipe_)
{
    if (!pipe_)
        return;

    // Pending inproc pipes enter the socket before a bind peer exists. Repeat
    // pair admission only after the registry has assigned the immutable peer
    // instance identity to this lane.
    attach_pipe (pipe_, false, true, true);
}

void zlink::socket_base_t::emit_socket_monitor_value_event (
  uint64_t event_, uint64_t value_, const endpoint_uri_pair_t &endpoint_uri_pair_)
{
    uint64_t values[1] = {value_};
    event (endpoint_uri_pair_, NULL, 0, values, 1, event_);
}

void zlink::socket_base_t::emit_peer_weight_changed (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return;

    const blob_t &routing_id = pipe_->get_routing_id ();
    const unsigned char *routing_id_data = routing_id.size () > 0 ? routing_id.data () : NULL;
    uint64_t values[1] = {static_cast<uint64_t> (weight_)};
    event (pipe_->get_endpoint_pair (), routing_id_data, routing_id.size (), values, 1,
           ZLINK_EVENT_PEER_WEIGHT_CHANGED);
}

void zlink::socket_base_t::event (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                  const unsigned char *routing_id_,
                                  size_t routing_id_size_,
                                  uint64_t values_[],
                                  uint64_t values_count_,
                                  uint64_t type_,
                                  uint32_t internal_flags_)
{
    if (monitor_runtime ().events_atomic.load (std::memory_order_acquire) == 0)
        return;

    bool enqueue = false;
    monitor_event_record_t record;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        if (monitor_runtime ().events & type_) {
            enqueue = build_monitor_event_record (
              &record, type_, values_, values_count_, routing_id_,
              routing_id_size_, endpoint_uri_pair_);
            if (enqueue)
                record.internal_flags = internal_flags_;
        }
    }
    //  A reliable internal monitor waits for its bounded queue. Keep the
    //  monitor lifecycle lock out of that wait so stop_monitor can wake it.
    if (enqueue)
        enqueue_monitor_event (record);
}

void zlink::socket_base_t::monitor_task_main (void *arg_)
{
    static_cast<socket_base_t *> (arg_)->pump_monitor_events ();
}

void zlink::socket_base_t::pump_monitor_events ()
{
    monitor_runtime_t &monitor = monitor_runtime ();
    void *monitor_socket = monitor.socket;
    monitor_event_record_t record;
    if (debug_env_enabled ("ZLINK_MONITOR_TASK_DIAG"))
        fprintf (stderr, "raw-monitor-task source-socket=%p\n", this);
    while (monitor.dequeue_worker_event_nowait (&record)) {
        bool delivered = true;
        if (monitor_socket)
            delivered = dispatch_monitor_event (monitor_socket, record);
        if (!delivered && !monitor.lossy) {
            monitor.requeue_worker_event_front (record);
            break;
        }
    }
}

void zlink::socket_base_t::enqueue_monitor_event (const monitor_event_record_t &record_)
{
    monitor_runtime ().enqueue_worker_event (record_, static_cast<size_t> (monitor_queue_hwm));
}

bool zlink::socket_base_t::build_monitor_event_record (
  monitor_event_record_t *out_,
  uint64_t event_,
  const uint64_t values_[],
  uint64_t values_count_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  const endpoint_uri_pair_t &endpoint_uri_pair_) const
{
    if (!out_ || values_count_ > monitor_max_values
        || routing_id_size_ > sizeof (out_->routing_id.data))
        return false;

    out_->event = event_;
    out_->values_count = values_count_;
    memset (out_->values, 0, sizeof (out_->values));
    for (uint64_t i = 0; i < values_count_; ++i)
        out_->values[i] = values_[i];
    memset (&out_->routing_id, 0, sizeof (out_->routing_id));
    out_->routing_id.size = static_cast<uint8_t> (routing_id_size_);
    if (routing_id_size_ > 0 && routing_id_)
        memcpy (out_->routing_id.data, routing_id_, routing_id_size_);
    out_->endpoint_uri_pair = endpoint_uri_pair_;
    return true;
}

bool zlink::socket_base_t::dispatch_monitor_event (void *monitor_socket_,
                                                   const monitor_event_record_t &record_) const
{
    if (!monitor_socket_)
        return false;

    socket_monitor_internal_event_t wire_event;
    memset (&wire_event, 0, sizeof (wire_event));
    wire_event.event.event = record_.event;
    if (record_.values_count > 0)
        wire_event.event.value = record_.values[0];
    wire_event.event.routing_id = record_.routing_id;
    wire_event.connection_id = record_.endpoint_uri_pair.connection_id;
    wire_event.internal_flags = record_.internal_flags;

    zlink::copy_fixed_c_string_from_bytes (wire_event.event.local_addr,
                                           sizeof (wire_event.event.local_addr),
                                           record_.endpoint_uri_pair.local.data (),
                                           record_.endpoint_uri_pair.local.size ());
    zlink::copy_fixed_c_string_from_bytes (wire_event.event.remote_addr,
                                           sizeof (wire_event.event.remote_addr),
                                           record_.endpoint_uri_pair.remote.data (),
                                           record_.endpoint_uri_pair.remote.size ());

    zlink_msg_t msg;
    zlink_msg_init_size (&msg, sizeof (wire_event));
    memcpy (zlink_msg_data (&msg), &wire_event, sizeof (wire_event));
    const int send_flags = monitor_runtime ().lossy ? ZLINK_DONTWAIT : 0;
    if (zlink::send_msg_internal (monitor_socket_, &msg, send_flags) == -1) {
        zlink_msg_close (&msg);
        return false;
    }
    return true;
}

void zlink::socket_base_t::stop_monitor (bool send_monitor_stopped_event_)
{
    socket_base_t *monitor_socket = detach_monitor_socket (send_monitor_stopped_event_);
    if (monitor_socket)
        zlink_close (monitor_socket);
}

zlink::socket_base_t *
zlink::socket_base_t::detach_monitor_socket (bool send_monitor_stopped_event_)
{
    monitor_runtime_t &monitor = monitor_runtime ();
    scoped_lock_t lock (monitor.sync);
    if (monitor.socket) {
        monitor.events_atomic.store (0, std::memory_order_release);
        socket_base_t *monitor_socket = static_cast<socket_base_t *> (monitor.socket);
        bool can_emit_monitor_stopped = false;
        const bool stop_async_mailbox = lifecycle_coordinator ().is_monitor_async_mailbox_owned ()
                                        && !socket_msg_dispatch_active () && !sub_dispatch_active ()
                                        && !xpub_dispatch_active () && !stream_dispatch_active ();

        if ((monitor.events & ZLINK_EVENT_MONITOR_STOPPED) && send_monitor_stopped_event_) {
            monitor_socket->process_commands (0, false);
            can_emit_monitor_stopped = monitor_socket->endpoint_runtime ().has_attached_pipes ();
        }

        if (monitor.task_id != 0) {
            control_runtime_t *runtime = get_ctx ()->control_runtime ();
            if (runtime)
                (void) runtime->remove_task (monitor.task_id);
        }
        monitor.stop_task ();

        if (can_emit_monitor_stopped) {
            uint64_t values[1] = {0};
            monitor_event_record_t record;
            if (build_monitor_event_record (&record, ZLINK_EVENT_MONITOR_STOPPED, values, 1, NULL,
                                            0, endpoint_uri_pair_t ()))
                dispatch_monitor_event (monitor.socket, record);
        }
        monitor.socket = NULL;
        monitor.events = 0;
        monitor.lossy = true;
        if (stop_async_mailbox) {
            stop_async_mailbox_processing ();
            wait_async_quiesced (10000);
        }
        lifecycle_coordinator ().set_monitor_async_mailbox_owned (false);
        return monitor_socket;
    }
    return NULL;
}
