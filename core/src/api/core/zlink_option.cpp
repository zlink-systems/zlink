/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/config_result_internal.hpp"
#include "api/core/zlink_option_internal.hpp"
#include "utils/err.hpp"

#include "core/ctx.hpp"

#include <stddef.h>
#include <string.h>

zlink::socket_base_t *as_socket (void *handle_)
{
    if (!handle_) {
        errno = EFAULT;
        return NULL;
    }
    zlink::socket_base_t *socket = static_cast<zlink::socket_base_t *> (handle_);
    if (!socket->check_tag ()) {
        errno = EFAULT;
        return NULL;
    }
    return socket;
}

int socket_type_of (zlink::socket_base_t *socket_)
{
    int type = 0;
    size_t size = sizeof (type);
    if (!socket_ || socket_->getsockopt (ZLINK_INTERNAL_OPT_TYPE, &type, &size) != 0) {
        return -1;
    }
    return type;
}

int set_socket_option_checked (zlink::socket_base_t *socket_,
                               int type_,
                               int expected_a_,
                               int expected_b_,
                               int option_,
                               const void *optval_,
                               size_t optvallen_)
{
    if (!socket_) {
        errno = EFAULT;
        return -1;
    }
    if (type_ != expected_a_ && type_ != expected_b_) {
        errno = EINVAL;
        return -1;
    }
    return socket_->setsockopt (option_, optval_, optvallen_);
}

int get_socket_option_checked (zlink::socket_base_t *socket_,
                               int type_,
                               int expected_a_,
                               int expected_b_,
                               int option_,
                               void *optval_,
                               size_t *optvallen_)
{
    if (!socket_) {
        errno = EFAULT;
        return -1;
    }
    if (type_ != expected_a_ && type_ != expected_b_) {
        errno = EINVAL;
        return -1;
    }
    return socket_->getsockopt (option_, optval_, optvallen_);
}

zlink::option_target_t::option_target_t () : kind (option_target_invalid), socket (NULL)
{
}

zlink::option_target_t zlink::resolve_option_target (void *handle_)
{
    option_target_t target;
    if (!handle_) {
        errno = EFAULT;
        return target;
    }

    target.socket = as_socket (handle_);
    if (target.socket)
        target.kind = option_target_socket;
    return target;
}

zlink_config_result_t
zlink_set_option (void *handle_, zlink_option_t option_, const void *optval_, size_t optvallen_)
{
    const option_descriptor_t *descriptor = lookup_common_option (option_);
    if (!descriptor)
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    const int socket_option = descriptor->internal_option;
    if (option_ == ZLINK_OPT_LAST_ENDPOINT || option_ == ZLINK_OPT_FD || option_ == ZLINK_OPT_EVENTS
        || option_ == ZLINK_OPT_TYPE) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        if (descriptor->unsupported_on_socket) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        return zlink::config_result_internal::from_rc (
          target.socket->setsockopt (socket_option, optval_, optvallen_));
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}

zlink_config_result_t
zlink_get_option (void *handle_, zlink_option_t option_, void *optval_, size_t *optvallen_)
{
    const option_descriptor_t *descriptor = lookup_common_option (option_);
    if (!descriptor)
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    const int socket_option = descriptor->internal_option;

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        if (descriptor->unsupported_on_socket) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        return zlink::config_result_internal::from_rc (
          target.socket->getsockopt (socket_option, optval_, optvallen_));
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}

zlink_config_result_t zlink_set_routing_id (void *handle_, const void *data_, size_t size_)
{
    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        const int type = socket_type_of (target.socket);
        if (type == ZLINK_CORE_SOCKET_STREAM) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        return zlink::config_result_internal::from_rc (
          target.socket->setsockopt (ZLINK_INTERNAL_OPT_ROUTING_ID, data_, size_));
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}

zlink_config_result_t zlink_get_routing_id (void *handle_, zlink_routing_id_t *out_)
{
    if (!out_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        size_t size = sizeof (out_->data);
        const int rc =
          target.socket->getsockopt (ZLINK_INTERNAL_OPT_ROUTING_ID, out_->data, &size);
        if (rc != 0)
            return zlink::config_result_internal::from_rc (rc);
        if (size > sizeof (out_->data)) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        out_->size = static_cast<uint8_t> (size);
        return ZLINK_CONFIG_OK;
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}

zlink_config_result_t
zlink_set_tls_server (void *handle_, const char *cert_, const char *key_, int require_client_cert_)
{
    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        if (!cert_ || !key_) {
            errno = EFAULT;
            return ZLINK_CONFIG_INVALID_HANDLE;
        }

        if (target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_CERT, cert_, strlen (cert_)) != 0
            || target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_KEY, key_, strlen (key_)) != 0
            || target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT,
                                          &require_client_cert_, sizeof (require_client_cert_))
                 != 0) {
            return zlink::config_result_internal::from_rc (-1);
        }

        return ZLINK_CONFIG_OK;
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}

zlink_config_result_t
zlink_set_tls_client (void *handle_, const char *ca_cert_, const char *hostname_, int trust_system_)
{
    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        if (!ca_cert_ || !hostname_) {
            errno = EFAULT;
            return ZLINK_CONFIG_INVALID_HANDLE;
        }

        if (target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_CA, ca_cert_, strlen (ca_cert_)) != 0
            || target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_HOSTNAME, hostname_,
                                          strlen (hostname_))
                 != 0
            || target.socket->setsockopt (ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM, &trust_system_,
                                          sizeof (trust_system_))
                 != 0) {
            return zlink::config_result_internal::from_rc (-1);
        }

        return ZLINK_CONFIG_OK;
    }
    errno = EFAULT;
    return ZLINK_CONFIG_INVALID_HANDLE;
}
