/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/config_result_internal.hpp"
#include "api/monitoring/monitor_api_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/socket_api_internal.hpp"

#include <cstring>

#include "core/c_api_copy_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/scoped_msg.hpp"

namespace
{
int recv_monitor_frame (void *monitor_socket_, zlink::scoped_msg_t *frame_, int flags_)
{
    if (!frame_) {
        errno = EFAULT;
        return -1;
    }

    return zlink::recv_msg_internal (monitor_socket_, frame_->get (), flags_);
}

bool copy_u64_frame (const zlink::scoped_msg_t &frame_, uint64_t *out_)
{
    if (!out_ || zlink_msg_size (frame_.get ()) < sizeof (*out_)) {
        errno = EPROTO;
        return false;
    }

    memcpy (out_, zlink_msg_data (const_cast<zlink_msg_t *> (frame_.get ())), sizeof (*out_));
    return true;
}
}

int socket_monitor_snapshot_provider (void *subject_, zlink_monitor_status_t *out_)
{
    zlink::socket_base_t *socket = static_cast<zlink::socket_base_t *> (subject_);
    if (!socket || !out_) {
        errno = EINVAL;
        return -1;
    }
    return socket->monitor_snapshot (out_);
}

int recv_socket_monitor_event_internal (void *monitor_socket_,
                                        zlink_monitor_event_t *event_,
                                        uint64_t *connection_id_out_,
                                        uint32_t *internal_flags_out_,
                                        int flags_)
{
    if (!monitor_socket_ || !event_) {
        errno = EINVAL;
        return -1;
    }

    zlink::scoped_msg_t first_frame;
    int rc = recv_monitor_frame (monitor_socket_, &first_frame, flags_);
    if (rc == -1)
        return -1;

    memset (event_, 0, sizeof (*event_));
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (internal_flags_out_)
        *internal_flags_out_ = 0;

    if (zlink_msg_size (first_frame.get ())
        >= sizeof (socket_monitor_internal_event_t)) {
        socket_monitor_internal_event_t internal_event;
        memcpy (&internal_event, zlink_msg_data (first_frame.get ()),
                sizeof (internal_event));
        *event_ = internal_event.event;
        if (connection_id_out_)
            *connection_id_out_ = internal_event.connection_id;
        if (internal_flags_out_)
            *internal_flags_out_ = internal_event.internal_flags;
        return 0;
    }
    if (zlink_msg_size (first_frame.get ()) >= sizeof (*event_)) {
        memcpy (event_, zlink_msg_data (first_frame.get ()), sizeof (*event_));
        return 0;
    }

    if (!copy_u64_frame (first_frame, &event_->event))
        return -1;

    const int follow_flags = flags_ & ~ZLINK_DONTWAIT;

    uint64_t value_count = 0;
    {
        zlink::scoped_msg_t value_count_frame;
        rc = recv_monitor_frame (monitor_socket_, &value_count_frame, follow_flags);
        if (rc == -1)
            return -1;
        if (!copy_u64_frame (value_count_frame, &value_count))
            return -1;
    }

    for (uint64_t i = 0; i < value_count; ++i) {
        zlink::scoped_msg_t value_frame;
        rc = recv_monitor_frame (monitor_socket_, &value_frame, follow_flags);
        if (rc == -1)
            return -1;
        if (i == 0 && zlink_msg_size (value_frame.get ()) >= sizeof (uint64_t))
            memcpy (&event_->value, zlink_msg_data (value_frame.get ()), sizeof (uint64_t));
    }

    {
        zlink::scoped_msg_t routing_id_frame;
        rc = recv_monitor_frame (monitor_socket_, &routing_id_frame, follow_flags);
        if (rc == -1)
            return -1;
        zlink::copy_routing_id_from_msg (routing_id_frame.ref (), &event_->routing_id);
    }

    {
        zlink::scoped_msg_t local_addr_frame;
        rc = recv_monitor_frame (monitor_socket_, &local_addr_frame, follow_flags);
        if (rc == -1)
            return -1;
        zlink::copy_fixed_c_string_from_bytes (event_->local_addr, sizeof (event_->local_addr),
                                               zlink_msg_data (local_addr_frame.get ()),
                                               zlink_msg_size (local_addr_frame.get ()));
    }

    {
        zlink::scoped_msg_t remote_addr_frame;
        rc = recv_monitor_frame (monitor_socket_, &remote_addr_frame, follow_flags);
        if (rc == -1)
            return -1;
        zlink::copy_fixed_c_string_from_bytes (event_->remote_addr, sizeof (event_->remote_addr),
                                               zlink_msg_data (remote_addr_frame.get ()),
                                               zlink_msg_size (remote_addr_frame.get ()));
    }

    return 0;
}

int recv_socket_monitor_event_unchecked (void *monitor_socket_,
                                         zlink_monitor_event_t *event_,
                                         int flags_)
{
    return recv_socket_monitor_event_internal (
      monitor_socket_, event_, NULL, NULL, flags_);
}

zlink_recv_result_t zlink_socket_monitor_recv (void *monitor_,
                                               zlink_socket_monitor_event_t *out_,
                                               zlink_recv_flags_t flags_)
{
    if (require_monitor_recv_model (monitor_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    return zlink::recv_result_internal::from_rc (
      recv_socket_monitor_event_unchecked (monitor_, out_, static_cast<int> (flags_)));
}

zlink_config_result_t zlink_monitor_status (void *monitor_, zlink_monitor_status_t *out_)
{
    if (!out_) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }

    socket_handle_t handle = as_socket_handle (monitor_);
    if (!handle.socket)
        return ZLINK_CONFIG_INVALID_HANDLE;

    //  The pin keeps the registry state alive for the whole snapshot call:
    //  a concurrent monitor close waits for it before deleting the state.
    monitor_state_pin_t pin (handle.socket);
    monitor_handler_state_t *state = pin.get ();
    if (!state) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }

    monitor_snapshot_provider_fn provider =
      state->snapshot_provider.load (std::memory_order_acquire);
    void *subject = state->snapshot_subject.load (std::memory_order_acquire);
    if (!provider) {
        errno = ENOTSUP;
        return ZLINK_CONFIG_NOT_SUPPORTED;
    }

    return zlink::config_result_internal::from_rc (provider (subject, out_));
}
