/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/poller_api_internal.hpp"
#include "api/core/zlink_option_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/message/handler_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"

namespace
{
int socket_send_ready_handler_internal (void *s_,
                                        zlink_send_ready_handler_fn handler_,
                                        void *userdata_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return -1;

    if (!handler_) {
        errno = EINVAL;
        return -1;
    }

    const int type = socket_type (handle);
    switch (type) {
        case ZLINK_CORE_SOCKET_PAIR:
        case ZLINK_CORE_SOCKET_PUB:
        case ZLINK_CORE_SOCKET_XPUB:
        case ZLINK_CORE_SOCKET_DEALER:
        case ZLINK_CORE_SOCKET_ROUTER:
        case ZLINK_CORE_SOCKET_STREAM:
            break;
        default:
            errno = ENOTSUP;
            return -1;
    }

    return handle.socket->socket_set_send_ready_handler_with_userdata (handler_, NULL, userdata_);
}
}

zlink_handler_result_t
zlink_recv_handler (void *s_, zlink_socket_msg_handler_fn handler_, void *userdata_)
{
    if (!handler_) {
        errno = EINVAL;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::handler_result_internal::from_errno (EFAULT);

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return ZLINK_HANDLER_NOT_SUPPORTED;
    }

    return zlink::handler_result_internal::from_rc (
      handle.socket->stream_set_msg_handler_with_userdata (handler_, userdata_));
}

zlink_handler_result_t zlink_stream_packet_handler (void *stream_,
                                                    zlink_stream_packet_handler_fn handler_,
                                                    void *userdata_)
{
    if (!handler_) {
        errno = EINVAL;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }

    socket_handle_t handle = as_socket_handle (stream_);
    if (!handle.socket)
        return zlink::handler_result_internal::from_errno (EFAULT);

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return ZLINK_HANDLER_NOT_SUPPORTED;
    }

    return zlink::handler_result_internal::from_rc (
      handle.socket->stream_set_packet_msg_handler_with_userdata (handler_, userdata_));
}

zlink_handler_result_t
zlink_send_ready_handler (void *s_, zlink_send_ready_handler_fn handler_, void *userdata_)
{
    if (!handler_) {
        errno = EINVAL;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }

    const zlink::option_target_t target = zlink::resolve_option_target (s_);
    if (target.kind == zlink::option_target_socket)
        return zlink::handler_result_internal::from_rc (
          socket_send_ready_handler_internal (s_, handler_, userdata_));

    errno = EFAULT;
    return ZLINK_HANDLER_INVALID_HANDLE;
}

int validate_socket_callback_poller_events (socket_handle_t handle_, short events_)
{
    if (!handle_.socket)
        return 0;
    const int type = socket_type (handle_);
    if ((events_ & ZLINK_POLLIN) != 0) {
        if (handle_.socket->socket_msg_dispatch_active ()
            || ((type == ZLINK_CORE_SOCKET_SUB || type == ZLINK_CORE_SOCKET_XSUB)
                && handle_.socket->sub_dispatch_active ())
            || (type == ZLINK_CORE_SOCKET_STREAM && handle_.socket->stream_dispatch_active ())) {
            errno = EBUSY;
            return -1;
        }
    }
    return 0;
}
