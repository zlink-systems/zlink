/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"
#include "api/message/handler_result_internal.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

extern "C" void zlink_socket_request_reply_cleanup (void *socket_)
{
    reqrep::cleanup_request_reply_socket (as_socket_handle (socket_));
}

extern "C" int zlink_router_enable_request_reply_receive (void *router_)
{
    if (reqrep::validate_socket_type (router_, ZLINK_CORE_SOCKET_ROUTER) != 0)
        return -1;

    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket) {
        errno = EFAULT;
        return -1;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle);
    return reqrep::ensure_completion_queue_ready (state);
}

zlink_handler_result_t zlink_router_completion_control_handler (
  void *router_, zlink_completion_control_handler_fn handler_, void *userdata_)
{
    if (!handler_) {
        errno = EINVAL;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket) {
        errno = EFAULT;
        return ZLINK_HANDLER_INVALID_HANDLE;
    }
    if (socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return ZLINK_HANDLER_NOT_SUPPORTED;
    }

    std::unique_ptr<zlink::socket_public_api_scope_t> admission =
      handle.socket->begin_public_api_scope ();
    if (!admission)
        return zlink::handler_result_internal::from_errno (errno);

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle);
    if (!state)
        return zlink::handler_result_internal::from_errno (errno);

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (state->closing) {
            errno = ETERM;
            return zlink::handler_result_internal::from_errno (errno);
        }
        state->completion_control_handler = handler_;
        state->completion_control_userdata = userdata_;
    }
    errno = 0;
    return ZLINK_HANDLER_OK;
}

extern "C" int zlink_socket_request_reply_set_default_timeout (void *socket_,
                                                               const void *optval_,
                                                               size_t optvallen_)
{
    const socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket) {
        errno = EINVAL;
        return -1;
    }

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_ROUTER && type != ZLINK_CORE_SOCKET_DEALER) {
        errno = EINVAL;
        return -1;
    }
    if (!optval_ || optvallen_ != sizeof (int)) {
        errno = EINVAL;
        return -1;
    }

    int timeout_ms = 0;
    memcpy (&timeout_ms, optval_, sizeof (timeout_ms));
    if (timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle);
    std::lock_guard<std::mutex> lock (state->mutex);
    state->default_timeout_ms = static_cast<uint32_t> (timeout_ms);
    return 0;
}

extern "C" int
zlink_socket_request_reply_get_default_timeout (void *socket_, void *optval_, size_t *optvallen_)
{
    const socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket) {
        errno = EINVAL;
        return -1;
    }

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_ROUTER && type != ZLINK_CORE_SOCKET_DEALER) {
        errno = EINVAL;
        return -1;
    }
    if (!optval_ || !optvallen_ || *optvallen_ < sizeof (int)) {
        errno = EINVAL;
        return -1;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle);
    int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        timeout_ms = static_cast<int> (state->default_timeout_ms);
    }

    memcpy (optval_, &timeout_ms, sizeof (timeout_ms));
    *optvallen_ = sizeof (timeout_ms);
    return 0;
}
