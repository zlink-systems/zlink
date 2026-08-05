/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/address.hpp"
#include "core/ctx_inproc_registry.hpp"
#include "sockets/common/socket_base.hpp"
#include "transports/ipc/ipc_address.hpp"

#include "transports/tcp/asio_tcp_listener.hpp"
#if defined ZLINK_HAVE_IPC
#include "transports/ipc/asio_ipc_listener.hpp"
#endif
#if defined ZLINK_HAVE_ASIO_SSL
#include "transports/tls/asio_tls_listener.hpp"
#endif
#if defined ZLINK_HAVE_WS
#include "transports/ws/asio_ws_listener.hpp"
#include "transports/ws/ws_address.hpp"
#endif
#ifdef ZLINK_HAVE_WSS
#include "transports/tls/wss_address.hpp"
#endif

int zlink::socket_base_t::bind_inproc_endpoint (const char *endpoint_uri_)
{
    const endpoint_t endpoint (this, options);
    const int rc = register_endpoint (endpoint_uri_, endpoint);
    if (rc == 0) {
        connect_pending (endpoint_uri_, this);
        endpoint_runtime ().set_last_endpoint (endpoint_uri_);
        options.connected = true;
    }
    return rc;
}

int zlink::socket_base_t::bind_transport_listener (const std::string &protocol_,
                                                   const std::string &address_,
                                                   io_thread_t *io_thread_)
{
    if (protocol_ == protocol_name::tcp) {
        asio_tcp_listener_t *listener =
          new (std::nothrow) asio_tcp_listener_t (io_thread_, this, options);
        alloc_assert (listener);
        int rc = listener->set_local_address (address_.c_str ());
        if (rc != 0) {
            LIBZLINK_DELETE (listener);
            event_bind_failed (make_unconnected_bind_endpoint_pair (address_), zlink_errno ());
            return -1;
        }

        std::string last_endpoint;
        listener->get_local_address (last_endpoint);
        endpoint_runtime ().set_last_endpoint (last_endpoint);
        add_endpoint (
          make_unconnected_bind_endpoint_pair (endpoint_runtime ().last_endpoint_uri ()),
          static_cast<own_t *> (listener), NULL);
        options.connected = true;
        return 0;
    }

#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
    if (protocol_ == protocol_name::tls) {
        asio_tls_listener_t *listener =
          new (std::nothrow) asio_tls_listener_t (io_thread_, this, options);
        alloc_assert (listener);
        int rc = listener->set_local_address (address_.c_str ());
        if (rc != 0) {
            LIBZLINK_DELETE (listener);
            event_bind_failed (make_unconnected_bind_endpoint_pair (address_), zlink_errno ());
            return -1;
        }

        std::string last_endpoint;
        listener->get_local_address (last_endpoint);
        endpoint_runtime ().set_last_endpoint (last_endpoint);
        add_endpoint (
          make_unconnected_bind_endpoint_pair (endpoint_runtime ().last_endpoint_uri ()),
          static_cast<own_t *> (listener), NULL);
        options.connected = true;
        return 0;
    }
#endif

#if defined ZLINK_HAVE_IPC
    if (protocol_ == protocol_name::ipc) {
        asio_ipc_listener_t *listener =
          new (std::nothrow) asio_ipc_listener_t (io_thread_, this, options);
        alloc_assert (listener);
        int rc = listener->set_local_address (address_.c_str ());
        if (rc != 0) {
            LIBZLINK_DELETE (listener);
            event_bind_failed (make_unconnected_bind_endpoint_pair (address_), zlink_errno ());
            return -1;
        }

        std::string last_endpoint;
        listener->get_local_address (last_endpoint);
        endpoint_runtime ().set_last_endpoint (last_endpoint);
        add_endpoint (
          make_unconnected_bind_endpoint_pair (endpoint_runtime ().last_endpoint_uri ()),
          static_cast<own_t *> (listener), NULL);
        options.connected = true;
        return 0;
    }
#endif

#if defined ZLINK_HAVE_WS
    if (protocol_ == protocol_name::ws
#if defined ZLINK_HAVE_WSS
        || protocol_ == protocol_name::wss
#endif
    ) {
        const bool secure =
#if defined ZLINK_HAVE_WSS
          protocol_ == protocol_name::wss;
#else
          false;
#endif

        ws_address_t *ws_addr =
#if defined ZLINK_HAVE_WSS
          secure ? static_cast<ws_address_t *> (new (std::nothrow) wss_address_t ()) :
#endif
                 new (std::nothrow) ws_address_t ();
        alloc_assert (ws_addr);
        int rc = ws_addr->resolve (address_.c_str (), true, options.ipv6);
        if (rc != 0) {
            LIBZLINK_DELETE (ws_addr);
            event_bind_failed (make_unconnected_bind_endpoint_pair (address_), zlink_errno ());
            return -1;
        }

        asio_ws_listener_t *listener =
          new (std::nothrow) asio_ws_listener_t (io_thread_, this, options);
        alloc_assert (listener);
        rc = listener->set_local_address (ws_addr, secure);
        LIBZLINK_DELETE (ws_addr);
        if (rc != 0) {
            LIBZLINK_DELETE (listener);
            event_bind_failed (make_unconnected_bind_endpoint_pair (address_), zlink_errno ());
            return -1;
        }

        std::string last_endpoint;
        listener->get_local_address (last_endpoint);
        endpoint_runtime ().set_last_endpoint (last_endpoint);
        add_endpoint (
          make_unconnected_bind_endpoint_pair (endpoint_runtime ().last_endpoint_uri ()),
          static_cast<own_t *> (listener), NULL);
        options.connected = true;
        return 0;
    }
#endif

    zlink_assert (false);
    errno = EPROTONOSUPPORT;
    return -1;
}

int zlink::socket_base_t::resolve_connect_address (const std::string &protocol_,
                                                   const std::string &address_,
                                                   address_t *paddr_) const
{
    int rc = 0;
    if (protocol_ == protocol_name::tcp
#ifdef ZLINK_HAVE_TLS
        || protocol_ == protocol_name::tls
#endif
    ) {
        const char *check = address_.c_str ();
        if (isalnum (*check) || isxdigit (*check) || *check == '[' || *check == ':') {
            check++;
            while (isalnum (*check) || isxdigit (*check) || *check == '.' || *check == '-'
                   || *check == ':' || *check == '%' || *check == ';' || *check == '['
                   || *check == ']' || *check == '_' || *check == '*') {
                check++;
            }
        }
        rc = -1;
        if (*check == 0) {
            check = strrchr (address_.c_str (), ':');
            if (check) {
                check++;
                if (*check && isdigit (*check))
                    rc = 0;
            }
        }
        if (rc == -1) {
            errno = EINVAL;
            return -1;
        }
        paddr_->resolved.tcp_addr = NULL;
        return 0;
    }
#ifdef ZLINK_HAVE_WS
#ifdef ZLINK_HAVE_WSS
    else if (protocol_ == protocol_name::ws || protocol_ == protocol_name::wss) {
        if (protocol_ == protocol_name::wss) {
            paddr_->resolved.wss_addr = new (std::nothrow) wss_address_t ();
            alloc_assert (paddr_->resolved.wss_addr);
            rc = paddr_->resolved.wss_addr->resolve (address_.c_str (), false, options.ipv6);
        } else
#else
    else if (protocol_ == protocol_name::ws) {
#endif
        {
            paddr_->resolved.ws_addr = new (std::nothrow) ws_address_t ();
            alloc_assert (paddr_->resolved.ws_addr);
            rc = paddr_->resolved.ws_addr->resolve (address_.c_str (), false, options.ipv6);
        }

        if (rc != 0)
            return -1;
        return 0;
    }
#endif

#if defined ZLINK_HAVE_IPC
    else if (protocol_ == protocol_name::ipc) {
        paddr_->resolved.ipc_addr = new (std::nothrow) ipc_address_t ();
        alloc_assert (paddr_->resolved.ipc_addr);
        rc = paddr_->resolved.ipc_addr->resolve (address_.c_str ());
        if (rc != 0)
            return -1;
        return 0;
    }
#endif

    errno = EPROTONOSUPPORT;
    return -1;
}
