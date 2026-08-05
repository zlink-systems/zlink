/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_base.hpp"

namespace
{
std::mutex g_part_helper_mutex;
std::unordered_map<void *, std::shared_ptr<zlink::part_helper_internal::handle_state_t>>
  g_part_helper_state;

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
try_socket_owned_handle_state (void *handle_)
{
    zlink::socket_base_t *socket = try_as_socket (handle_);
    return socket ? socket->part_helper_state ()
                  : std::shared_ptr<zlink::part_helper_internal::handle_state_t> ();
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
create_socket_owned_handle_state (void *handle_)
{
    zlink::socket_base_t *socket = try_as_socket (handle_);
    if (!socket)
        return std::shared_ptr<zlink::part_helper_internal::handle_state_t> ();

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state =
      socket->part_helper_state ();
    if (state)
        return state;

    state.reset (new (std::nothrow) zlink::part_helper_internal::handle_state_t ());
    if (!state) {
        errno = ENOMEM;
        return std::shared_ptr<zlink::part_helper_internal::handle_state_t> ();
    }

    return socket->set_part_helper_state (state);
}

}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::part_helper_internal::find_or_create_handle_state (void *handle_)
{
    if (!handle_) {
        errno = EFAULT;
        return std::shared_ptr<handle_state_t> ();
    }

    std::shared_ptr<handle_state_t> socket_state = create_socket_owned_handle_state (handle_);
    if (socket_state)
        return socket_state;
    if (try_as_socket (handle_))
        return std::shared_ptr<handle_state_t> ();

    std::lock_guard<std::mutex> lock (g_part_helper_mutex);
    std::unordered_map<void *, std::shared_ptr<handle_state_t>>::iterator it =
      g_part_helper_state.find (handle_);
    if (it != g_part_helper_state.end ())
        return it->second;

    std::shared_ptr<handle_state_t> state (new (std::nothrow) handle_state_t ());
    if (!state) {
        errno = ENOMEM;
        return std::shared_ptr<handle_state_t> ();
    }

    g_part_helper_state[handle_] = state;
    return state;
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::part_helper_internal::find_handle_state (void *handle_)
{
    std::shared_ptr<handle_state_t> socket_state = try_socket_owned_handle_state (handle_);
    if (socket_state)
        return socket_state;
    if (try_as_socket (handle_))
        return std::shared_ptr<handle_state_t> ();

    std::lock_guard<std::mutex> lock (g_part_helper_mutex);
    std::unordered_map<void *, std::shared_ptr<handle_state_t>>::iterator it =
      g_part_helper_state.find (handle_);
    return it != g_part_helper_state.end () ? it->second : std::shared_ptr<handle_state_t> ();
}

void zlink::part_helper_internal::cleanup_handle (void *handle_)
{
    std::shared_ptr<handle_state_t> state;
    zlink::socket_base_t *socket = try_as_socket (handle_);
    if (socket) {
        state = socket->part_helper_state ();
        if (!state)
            return;
        socket->clear_part_helper_state ();
    }
    if (!state) {
        std::lock_guard<std::mutex> lock (g_part_helper_mutex);
        std::unordered_map<void *, std::shared_ptr<handle_state_t>>::iterator it =
          g_part_helper_state.find (handle_);
        if (it == g_part_helper_state.end ())
            return;
        state = it->second;
        g_part_helper_state.erase (it);
    }

    std::lock_guard<std::mutex> lock (state->mutex);
    reset_send_sequence (&state->send);
    reset_recv_sequence (&state->recv);
}
