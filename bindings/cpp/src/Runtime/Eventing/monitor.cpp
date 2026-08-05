/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Sockets/socket_contracts.hpp>

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Eventing/monitor_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>

#include <zlink.h>

#include <cerrno>

namespace zlink
{

namespace
{

monitor_event_t make_monitor_event (const zlink_monitor_event_t &native_)
{
    monitor_event_t event;
    event.event = static_cast<monitor_event> (native_.event);
    event.value = native_.value;
    event.routing_id =
      native_.routing_id.size > 0
        ? std::optional<routing_id_t> (detail::native_routing_id (native_.routing_id))
        : std::nullopt;
    event.local_addr = native_.local_addr;
    event.remote_addr = native_.remote_addr;
    return event;
}

monitor_status_t make_monitor_status (const zlink_monitor_status_t &native_)
{
    if (native_.abi_version != ZLINK_MONITOR_STATUS_ABI_VERSION
        || native_.struct_size != sizeof (zlink_monitor_status_t))
        throw config_error_t (config_result_t::invalid_state, EPROTO);

    monitor_status_t status;
    status.abi_version = native_.abi_version;
    status.struct_size = native_.struct_size;
    status.source_kind = static_cast<monitor_source_kind> (native_.source_kind);
    status.state_flags = native_.state_flags;
    status.detail_flags = native_.detail_flags;
    status.snd_pending_msgs = native_.snd_pending_msgs;
    status.rcv_pending_msgs = native_.rcv_pending_msgs;
    status.auto_hwm_enabled = native_.auto_hwm_enabled != 0;
    status.auto_hwm_profile = native_.auto_hwm_profile;
    status.auto_hwm_role = native_.auto_hwm_role;
    status.auto_hwm_policy_class = native_.auto_hwm_policy_class;
    status.auto_hwm_unit_budget_bytes = native_.auto_hwm_unit_budget_bytes;
    status.auto_hwm_size_cap = native_.auto_hwm_size_cap;
    status.auto_hwm_socket_message_slots = native_.auto_hwm_socket_message_slots;
    status.auto_hwm_connection_bucket_enabled = native_.auto_hwm_connection_bucket_enabled != 0;
    status.auto_hwm_connection_bucket_count = native_.auto_hwm_connection_bucket_count;
    status.auto_hwm_connection_bucket_index = native_.auto_hwm_connection_bucket_index;
    status.auto_hwm_connection_bucket_hwm_4k = native_.auto_hwm_connection_bucket_hwm_4k;
    status.auto_hwm_connection_bucket_hysteresis_retained =
      native_.auto_hwm_connection_bucket_hysteresis_retained != 0;
    status.auto_hwm_effective_message_bytes = native_.auto_hwm_effective_message_bytes;
    status.auto_hwm_planned_sndhwm_bytes = native_.auto_hwm_planned_sndhwm_bytes;
    status.auto_hwm_planned_rcvhwm_bytes = native_.auto_hwm_planned_rcvhwm_bytes;
    status.auto_hwm_applied_sndhwm_bytes = native_.auto_hwm_applied_sndhwm_bytes;
    status.auto_hwm_applied_rcvhwm_bytes = native_.auto_hwm_applied_rcvhwm_bytes;
    status.auto_hwm_effective_sndbuf = native_.auto_hwm_effective_sndbuf;
    status.auto_hwm_effective_rcvbuf = native_.auto_hwm_effective_rcvbuf;
    status.auto_hwm_last_recalc_ms = native_.auto_hwm_last_recalc_ms;
    status.auto_hwm_last_recalc_reason = native_.auto_hwm_last_recalc_reason;
    status.auto_hwm_send_blocked_ratio_ppm = native_.auto_hwm_send_blocked_ratio_ppm;
    status.auto_hwm_deferred_sndhwm_bytes = native_.auto_hwm_deferred_sndhwm_bytes;
    status.auto_hwm_deferred_rcvhwm_bytes = native_.auto_hwm_deferred_rcvhwm_bytes;
    status.auto_hwm_deferred_sndhwm_valid = native_.auto_hwm_deferred_sndhwm_valid != 0;
    status.auto_hwm_deferred_rcvhwm_valid = native_.auto_hwm_deferred_rcvhwm_valid != 0;
    status.snd_bytes_in_flight = native_.snd_bytes_in_flight;
    status.rcv_bytes_in_flight = native_.rcv_bytes_in_flight;
    status.minimum_core_message_charge_bytes = native_.minimum_core_message_charge_bytes;
    status.oversize_message_admission_count = native_.oversize_message_admission_count;
    status.oversize_message_admission_max_bytes = native_.oversize_message_admission_max_bytes;
    return status;
}

} // namespace

struct socket_monitor_t::impl
{
    void *handle = nullptr;
    std::function<void (const monitor_event_t &)> event_function_handler;
};

namespace detail
{

void *monitor_access_t::native_handle (socket_monitor_t &monitor_) noexcept
{
    return monitor_._impl ? monitor_._impl->handle : nullptr;
}

const void *monitor_access_t::native_handle (const socket_monitor_t &monitor_) noexcept
{
    return monitor_._impl ? monitor_._impl->handle : nullptr;
}

} // namespace detail

socket_monitor_t::socket_monitor_t () : _impl (std::make_unique<impl> ())
{
}

socket_monitor_t::~socket_monitor_t ()
{
    close_noexcept ();
}

socket_monitor_t::socket_monitor_t (socket_monitor_t &&other) noexcept = default;

socket_monitor_t &socket_monitor_t::operator= (socket_monitor_t &&other) noexcept
{
    if (this == &other)
        return *this;
    close_noexcept ();
    _impl = std::move (other._impl);
    return *this;
}

bool socket_monitor_t::valid () const noexcept
{
    return _impl && _impl->handle != nullptr;
}

socket_monitor_t socket_monitor_t::open (const socket_t &socket_, monitor_event events_)
{
    zlink_socket_monitor_open_options_t options;
    options.events = static_cast<zlink_socket_monitor_event_mask_t> (events_);
    socket_monitor_t out;
    out._impl->handle =
      zlink_socket_monitor_open (const_cast<void *> (detail::native_handle (socket_)), &options);
    if (!out._impl->handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    return out;
}

void socket_monitor_t::on_event (std::function<void (const monitor_event_t &)> handler_)
{
    _impl->event_function_handler = std::move (handler_);
    detail::throw_if_failed<handler_error_t> (
      static_cast<handler_result_t> (zlink_socket_monitor_handler (
        _impl->handle,
        [] (const zlink_monitor_event_t *event_, void *userdata_) {
            socket_monitor_t *self = static_cast<socket_monitor_t *> (userdata_);
            if (!self || !self->_impl || !self->_impl->event_function_handler || !event_)
                return;
            const monitor_event_t event = make_monitor_event (*event_);
            self->_impl->event_function_handler (event);
        },
        this)));
}

std::optional<monitor_event_t> socket_monitor_t::recv (recv_flags_t flags_)
{
    zlink_monitor_event_t event;
    const recv_result_t result = static_cast<recv_result_t> (zlink_socket_monitor_recv (
      _impl->handle, &event, static_cast<zlink_recv_flags_t> (static_cast<int> (flags_))));
    if (result == recv_result_t::no_data && flags_ == recv_flags_t::dontwait)
        return std::nullopt;
    if (result != recv_result_t::ok)
        throw recv_error_t (result, zlink_errno ());
    return std::optional<monitor_event_t> (make_monitor_event (event));
}

monitor_status_t socket_monitor_t::status () const
{
    zlink_monitor_status_t snapshot;
    detail::throw_if_failed<config_error_t> (
      static_cast<config_result_t> (zlink_monitor_status (_impl->handle, &snapshot)));
    return make_monitor_status (snapshot);
}

void socket_monitor_t::close ()
{
    if (!_impl || !_impl->handle)
        return;
    void *monitor = _impl->handle;
    const close_result_t result = static_cast<close_result_t> (zlink_monitor_close (&monitor));
    if (result != close_result_t::ok)
        throw close_error_t (result, zlink_errno ());
    _impl->handle = nullptr;
    _impl->event_function_handler = nullptr;
}

void socket_monitor_t::close_noexcept () noexcept
{
    if (!_impl || !_impl->handle)
        return;
    void *monitor = _impl->handle;
    (void) zlink_monitor_close (&monitor);
    _impl->handle = nullptr;
    _impl->event_function_handler = nullptr;
}

} // namespace zlink
