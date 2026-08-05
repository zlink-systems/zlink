/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>

#include "api/socket/socket_api_internal.hpp"
#include "api/message/bind_result_internal.hpp"
#include "api/message/connect_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "core/address.hpp"
#include "sockets/proxy/proxy.hpp"

#ifndef ZLINK_INTERNAL_EXPORT
#if defined _WIN32 && defined DLL_EXPORT && !defined ZLINK_STATIC
#define ZLINK_INTERNAL_EXPORT __declspec (dllexport)
#else
#define ZLINK_INTERNAL_EXPORT
#endif
#endif

zlink_bind_result_t zlink_bind (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::bind_result_internal::from_errno (EFAULT);
    const int rc = handle.socket->bind (addr_);
    if (rc != 0)
        return zlink::bind_result_internal::from_rc (rc);
    return zlink::bind_result_internal::from_rc (rc);
}

zlink_connect_result_t zlink_connect (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (EFAULT);
    const int rc = handle.socket->connect (addr_);
    if (rc != 0)
        return zlink::connect_result_internal::from_rc (rc);
    return zlink::connect_result_internal::from_rc (rc);
}

zlink_connect_result_t zlink_unbind (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (EFAULT);
    return zlink::connect_result_internal::from_rc (handle.socket->term_endpoint (addr_));
}

zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (EFAULT);
    return zlink::connect_result_internal::from_rc (handle.socket->term_endpoint (addr_));
}

zlink_connect_result_t zlink_disconnect_rid (void *s_, const zlink_routing_id_t *peer_rid_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::connect_result_internal::from_errno (EFAULT);
    return zlink::connect_result_internal::from_rc (handle.socket->term_peer_rid (peer_rid_));
}

int zlink_stream_attach_raw (void *s_, zlink_stream_on_raw_fn on_raw_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return -1;
    if (!on_raw_) {
        errno = EINVAL;
        return -1;
    }
    if (!is_stream_type (handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle.socket->stream_dispatch_in_callback ()) {
        errno = EBUSY;
        return -1;
    }

    stream_api_lock_t api_lock (handle);
    return handle.socket->stream_dispatch_start_raw (on_raw_);
}

ZLINK_INTERNAL_EXPORT int zlink_stream_detach (void *s_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return -1;
    if (!is_stream_type (handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle.socket->stream_dispatch_in_callback ()) {
        errno = EBUSY;
        return -1;
    }

    stream_api_lock_t api_lock (handle);
    return handle.socket->stream_dispatch_stop ();
}

zlink_config_result_t zlink_proxy (void *frontend_, void *backend_, void *capture_)
{
    if (!frontend_ || !backend_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    socket_handle_t frontend = as_socket_handle (frontend_);
    if (!frontend.socket)
        return ZLINK_CONFIG_INVALID_HANDLE;
    socket_handle_t backend = as_socket_handle (backend_);
    if (!backend.socket)
        return ZLINK_CONFIG_INVALID_HANDLE;

    zlink::socket_base_t *capture_socket = NULL;
    if (capture_) {
        socket_handle_t capture = as_socket_handle (capture_);
        if (!capture.socket)
            return ZLINK_CONFIG_INVALID_HANDLE;
        capture_socket = capture.socket;
    }

    return zlink::config_result_internal::from_rc (
      zlink::proxy (frontend.socket, backend.socket, capture_socket));
}

zlink_config_result_t
zlink_proxy_steerable (void *frontend_, void *backend_, void *capture_, void *control_)
{
    if (!frontend_ || !backend_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    socket_handle_t frontend = as_socket_handle (frontend_);
    if (!frontend.socket)
        return ZLINK_CONFIG_INVALID_HANDLE;
    socket_handle_t backend = as_socket_handle (backend_);
    if (!backend.socket)
        return ZLINK_CONFIG_INVALID_HANDLE;

    zlink::socket_base_t *capture_socket = NULL;
    if (capture_) {
        socket_handle_t capture = as_socket_handle (capture_);
        if (!capture.socket)
            return ZLINK_CONFIG_INVALID_HANDLE;
        capture_socket = capture.socket;
    }

    zlink::socket_base_t *control_socket = NULL;
    if (control_) {
        socket_handle_t control = as_socket_handle (control_);
        if (!control.socket)
            return ZLINK_CONFIG_INVALID_HANDLE;
        control_socket = control.socket;
    }

    return zlink::config_result_internal::from_rc (
      zlink::proxy_steerable (frontend.socket, backend.socket, capture_socket, control_socket));
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
