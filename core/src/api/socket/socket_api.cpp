/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>

#include "api/socket/socket_api_internal.hpp"
#include "api/message/bind_result_internal.hpp"
#include "api/message/connect_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "core/address.hpp"
#include "sockets/proxy/proxy.hpp"

zlink_bind_result_t zlink_bind (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::bind_result_internal::from_errno (errno);
    const int rc = handle.socket->bind (addr_);
    if (rc != 0)
        return zlink::bind_result_internal::from_rc (rc);
    return zlink::bind_result_internal::from_rc (rc);
}

zlink_connect_result_t zlink_connect (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (errno);
    const int rc = handle.socket->connect (addr_);
    if (rc != 0)
        return zlink::connect_result_internal::from_rc (rc);
    return zlink::connect_result_internal::from_rc (rc);
}

zlink_connect_result_t zlink_unbind (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (errno);
    return zlink::connect_result_internal::from_rc (handle.socket->term_endpoint (addr_));
}

zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (errno);
    return zlink::connect_result_internal::from_rc (handle.socket->term_endpoint (addr_));
}

zlink_connect_result_t zlink_disconnect_rid (void *s_, const zlink_routing_id_t *peer_rid_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (errno);
    return zlink::connect_result_internal::from_rc (handle.socket->term_peer_rid (peer_rid_));
}

zlink_config_result_t zlink_proxy (void *frontend_, void *backend_, void *capture_)
{
    if (!frontend_ || !backend_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    socket_handle_t frontend = as_socket_handle (frontend_);
    if (!frontend.socket)
        return zlink::config_result_internal::from_errno (errno);
    socket_handle_t backend = as_socket_handle (backend_);
    if (!backend.socket)
        return zlink::config_result_internal::from_errno (errno);

    socket_handle_t capture;
    if (capture_) {
        capture = as_socket_handle (capture_);
        if (!capture.socket)
            return zlink::config_result_internal::from_errno (errno);
    }

    return zlink::config_result_internal::from_rc (
      zlink::proxy (frontend.socket, backend.socket, capture.socket));
}

bool zlink_has (const char *capability_)
{
    if (strcmp (capability_, "tcp") == 0)
        return true;
#if defined(ZLINK_HAVE_IPC)
    if (strcmp (capability_, zlink::protocol_name::ipc) == 0)
        return true;
#endif
#ifdef ZLINK_HAVE_WS
    if (strcmp (capability_, zlink::protocol_name::ws) == 0)
        return true;
#endif
#ifdef ZLINK_HAVE_WSS
    if (strcmp (capability_, zlink::protocol_name::wss) == 0)
        return true;
#endif
#ifdef ZLINK_HAVE_TLS
    if (strcmp (capability_, zlink::protocol_name::tls) == 0)
        return true;
#endif
    return false;
}
