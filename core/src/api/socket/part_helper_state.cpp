/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <new>

#include "api/socket/part_helper_internal.hpp"
#include "sockets/common/socket_base.hpp"

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
        if (state->send.active)
            (void) try_rollback_send_scope_locked (&state->send);
        if (state->send.sink_socket)
            state->send.sink_socket->clear_incremental_send_control_boundary ();
        reset_send_sequence (&state->send, false);
        held_receive_socket = reset_recv_sequence (&state->recv);
    }
    if (held_receive_socket)
        held_receive_socket->end_public_part_receive_delivery_hold ();
    errno = saved_errno;
}
