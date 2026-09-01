/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/config_result_internal.hpp"
#include "api/core/zlink_option_internal.hpp"

extern "C" int zlink_socket_request_reply_set_default_timeout (void *socket_,
                                                               const void *optval_,
                                                               size_t optvallen_);
extern "C" int
zlink_socket_request_reply_get_default_timeout (void *socket_, void *optval_, size_t *optvallen_);

namespace
{
zlink_config_result_t invalid_option_argument ()
{
    errno = EINVAL;
    return ZLINK_CONFIG_INVALID_ARGUMENT;
}

zlink_config_result_t set_socket_only_option (void *handle_,
                                              int socket_option_,
                                              int expected_a_,
                                              int expected_b_,
                                              const void *optval_,
                                              size_t optvallen_)
{
    socket_handle_t handle = as_socket (handle_);
    zlink::socket_base_t *socket = handle.socket;
    if (!socket)
        return zlink::config_result_internal::from_errno (errno);
    return zlink::config_result_internal::from_rc (
      set_socket_option_checked (socket, socket_type_of (socket), expected_a_, expected_b_,
                                 socket_option_, optval_, optvallen_));
}

zlink_config_result_t get_socket_only_option (void *handle_,
                                              int socket_option_,
                                              int expected_a_,
                                              int expected_b_,
                                              void *optval_,
                                              size_t *optvallen_)
{
    socket_handle_t handle = as_socket (handle_);
    zlink::socket_base_t *socket = handle.socket;
    if (!socket)
        return zlink::config_result_internal::from_errno (errno);
    return zlink::config_result_internal::from_rc (
      get_socket_option_checked (socket, socket_type_of (socket), expected_a_, expected_b_,
                                 socket_option_, optval_, optvallen_));
}

zlink_config_result_t set_probe_option (zlink::socket_base_t *socket_,
                                        int socket_option_,
                                        const void *optval_,
                                        size_t optvallen_)
{
    if (!optval_ || optvallen_ != sizeof (int))
        return invalid_option_argument ();

    int enabled = 0;
    memcpy (&enabled, optval_, sizeof (enabled));
    if (enabled < 0)
        return invalid_option_argument ();

    // A connection probe is emitted from xattach_pipe(). Keep command
    // progress active after connect so the bind command reaches that owner
    // even when the application is blocked receiving on the peer socket.
    if (enabled != 0 && socket_->ensure_async_command_processing () != 0)
        return zlink::config_result_internal::from_errno (errno);

    return zlink::config_result_internal::from_rc (
      socket_->setsockopt (socket_option_, optval_, optvallen_));
}

zlink_config_result_t set_socket_request_timeout (void *handle_,
                                                  int expected_type_,
                                                  const void *optval_,
                                                  size_t optvallen_)
{
    socket_handle_t handle = as_socket (handle_);
    if (!handle.socket)
        return zlink::config_result_internal::from_errno (errno);
    if (socket_type_of (handle.socket) != expected_type_)
        return invalid_option_argument ();
    return zlink::config_result_internal::from_rc (
      zlink_socket_request_reply_set_default_timeout (handle_, optval_, optvallen_));
}

zlink_config_result_t
get_socket_request_timeout (void *handle_, int expected_type_, void *optval_, size_t *optvallen_)
{
    socket_handle_t handle = as_socket (handle_);
    if (!handle.socket)
        return zlink::config_result_internal::from_errno (errno);
    if (socket_type_of (handle.socket) != expected_type_)
        return invalid_option_argument ();
    return zlink::config_result_internal::from_rc (
      zlink_socket_request_reply_get_default_timeout (handle_, optval_, optvallen_));
}
}

zlink_config_result_t zlink_set_router_option (void *handle_,
                                               zlink_router_option_t option_,
                                               const void *optval_,
                                               size_t optvallen_)
{
    if (option_ == ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS) {
        return set_socket_request_timeout (handle_, ZLINK_CORE_SOCKET_ROUTER, optval_, optvallen_);
    }

    const int socket_option = map_router_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    socket_handle_t handle = as_socket (handle_);
    if (zlink::socket_base_t *socket = handle.socket) {
        const int type = socket_type_of (socket);
        if (type == ZLINK_CORE_SOCKET_DEALER && option_ != ZLINK_ROUTER_OPT_PROBE) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        if (type != ZLINK_CORE_SOCKET_ROUTER && type != ZLINK_CORE_SOCKET_DEALER) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        if (option_ == ZLINK_ROUTER_OPT_PROBE)
            return set_probe_option (socket, socket_option, optval_, optvallen_);
        return zlink::config_result_internal::from_rc (
          socket->setsockopt (socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t zlink_get_router_option (void *handle_,
                                               zlink_router_option_t option_,
                                               void *optval_,
                                               size_t *optvallen_)
{
    if (option_ == ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS) {
        return get_socket_request_timeout (handle_, ZLINK_CORE_SOCKET_ROUTER, optval_, optvallen_);
    }

    const int socket_option = map_router_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    socket_handle_t handle = as_socket (handle_);
    if (zlink::socket_base_t *socket = handle.socket) {
        const int type = socket_type_of (socket);
        if (type == ZLINK_CORE_SOCKET_DEALER && option_ != ZLINK_ROUTER_OPT_PROBE) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        if (type != ZLINK_CORE_SOCKET_ROUTER && type != ZLINK_CORE_SOCKET_DEALER) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        return zlink::config_result_internal::from_rc (
          socket->getsockopt (socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t zlink_set_dealer_option (void *handle_,
                                               zlink_dealer_option_t option_,
                                               const void *optval_,
                                               size_t optvallen_)
{
    if (option_ == ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS) {
        return set_socket_request_timeout (handle_, ZLINK_CORE_SOCKET_DEALER, optval_, optvallen_);
    }

    const int socket_option = map_dealer_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    if (option_ == ZLINK_DEALER_OPT_PROBE) {
        socket_handle_t handle = as_socket (handle_);
        if (!handle.socket)
            return zlink::config_result_internal::from_errno (errno);
        if (socket_type_of (handle.socket) != ZLINK_CORE_SOCKET_DEALER)
            return invalid_option_argument ();
        return set_probe_option (handle.socket, socket_option, optval_, optvallen_);
    }

    return set_socket_only_option (handle_, socket_option,
                                   ZLINK_CORE_SOCKET_DEALER,
                                   ZLINK_CORE_SOCKET_DEALER, optval_, optvallen_);
}

zlink_config_result_t zlink_get_dealer_option (void *handle_,
                                               zlink_dealer_option_t option_,
                                               void *optval_,
                                               size_t *optvallen_)
{
    if (option_ == ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS) {
        return get_socket_request_timeout (handle_, ZLINK_CORE_SOCKET_DEALER, optval_, optvallen_);
    }

    const int socket_option = map_dealer_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    return get_socket_only_option (handle_, socket_option, ZLINK_CORE_SOCKET_DEALER,
                                   ZLINK_CORE_SOCKET_DEALER, optval_, optvallen_);
}

zlink_config_result_t zlink_set_stream_option (void *handle_,
                                               zlink_stream_option_t option_,
                                               const void *optval_,
                                               size_t optvallen_)
{
    const int socket_option = map_stream_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    return set_socket_only_option (handle_, socket_option, ZLINK_CORE_SOCKET_STREAM,
                                   ZLINK_CORE_SOCKET_STREAM, optval_, optvallen_);
}

zlink_config_result_t zlink_get_stream_option (void *handle_,
                                               zlink_stream_option_t option_,
                                               void *optval_,
                                               size_t *optvallen_)
{
    const int socket_option = map_stream_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    return get_socket_only_option (handle_, socket_option, ZLINK_CORE_SOCKET_STREAM,
                                   ZLINK_CORE_SOCKET_STREAM, optval_, optvallen_);
}

zlink_config_result_t zlink_set_pub_option (void *handle_,
                                            zlink_pub_option_t option_,
                                            const void *optval_,
                                            size_t optvallen_)
{
    const int socket_option = map_pub_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        return zlink::config_result_internal::from_rc (
          set_socket_option_checked (target.socket, socket_type_of (target.socket),
                                     ZLINK_CORE_SOCKET_PUB,
                                     ZLINK_CORE_SOCKET_XPUB, socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}
zlink_config_result_t
zlink_get_pub_option (void *handle_, zlink_pub_option_t option_, void *optval_, size_t *optvallen_)
{
    const int socket_option = map_pub_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        return zlink::config_result_internal::from_rc (
          get_socket_option_checked (target.socket, socket_type_of (target.socket),
                                     ZLINK_CORE_SOCKET_PUB,
                                     ZLINK_CORE_SOCKET_XPUB, socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t zlink_set_sub_option (void *handle_,
                                            zlink_sub_option_t option_,
                                            const void *optval_,
                                            size_t optvallen_)
{
    const int socket_option = map_sub_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        return zlink::config_result_internal::from_rc (
          set_socket_option_checked (target.socket, socket_type_of (target.socket),
                                     ZLINK_CORE_SOCKET_SUB,
                                     ZLINK_CORE_SOCKET_XSUB, socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t
zlink_get_sub_option (void *handle_, zlink_sub_option_t option_, void *optval_, size_t *optvallen_)
{
    const int socket_option = map_sub_option (option_);
    if (socket_option < 0)
        return ZLINK_CONFIG_INVALID_ARGUMENT;

    const zlink::option_target_t target = zlink::resolve_option_target (handle_);
    if (target.kind == zlink::option_target_socket) {
        return zlink::config_result_internal::from_rc (
          get_socket_option_checked (target.socket, socket_type_of (target.socket),
                                     ZLINK_CORE_SOCKET_SUB,
                                     ZLINK_CORE_SOCKET_XSUB, socket_option, optval_, optvallen_));
    }
    return zlink::config_result_internal::from_errno (errno);
}
