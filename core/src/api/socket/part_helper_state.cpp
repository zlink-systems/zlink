/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <new>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_base.hpp"

namespace
{
bool prepare_send_scope_for_cleanup (
  zlink::part_helper_internal::send_sequence_state_t *send_)
{
    if (!send_ || !send_->send_scope)
        return false;
    return send_->send_scope->acquired ()
           || send_->send_scope->resume_multipart_call ()
           || send_->send_scope->lock_multipart_for_close_cleanup ();
}

}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::part_helper_internal::find_socket_state (socket_base_t *socket_)
{
    return socket_ ? socket_->part_helper_state ()
                   : std::shared_ptr<handle_state_t> ();
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::part_helper_internal::find_or_create_socket_state (socket_base_t *socket_)
{
    if (!socket_)
        return std::shared_ptr<handle_state_t> ();

    std::shared_ptr<handle_state_t> state = socket_->part_helper_state ();
    if (state)
        return state;

    try {
        state.reset (new (std::nothrow) handle_state_t ());
    } catch (...) {
        errno = ENOMEM;
        return std::shared_ptr<handle_state_t> ();
    }
    if (!state) {
        errno = ENOMEM;
        return std::shared_ptr<handle_state_t> ();
    }

    return socket_->set_part_helper_state (state);
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::part_helper_internal::find_handle_state (void *handle_)
{
    socket_handle_t handle = as_socket_handle (handle_);
    return handle.socket ? find_socket_state (handle.socket)
                         : std::shared_ptr<handle_state_t> ();
}

void zlink::part_helper_internal::cleanup_socket (socket_base_t *socket_)
{
    const int saved_errno = errno;
    if (!socket_) {
        errno = saved_errno;
        return;
    }

    std::shared_ptr<handle_state_t> state = socket_->part_helper_state ();
    if (!state) {
        errno = saved_errno;
        return;
    }
    socket_->clear_part_helper_state ();

    zlink::socket_base_t *held_receive_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (state->send.active && state->send.sink_socket
            && prepare_send_scope_for_cleanup (&state->send)) {
            (void) state->send.sink_socket->rollback_scoped (
              *state->send.send_scope);
        }
        if (state->send.sink_socket)
            state->send.sink_socket->clear_incremental_send_control_boundary ();
        reset_send_sequence (&state->send, false);
        held_receive_socket = reset_recv_sequence (&state->recv);
    }
    if (held_receive_socket)
        held_receive_socket->end_public_part_receive_delivery_hold ();
    errno = saved_errno;
}
