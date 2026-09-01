/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

void zlink::socket_dispatch_bridge_t::mark_send_recovery_pending ()
{
    send_recovery_pending_flag.store (true, std::memory_order_release);
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_pending ()
{
    send_recovery_pending_flag.store (false, std::memory_order_release);
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::mark_send_recovery_ready ()
{
    if (!send_recovery_pending_flag.load (std::memory_order_acquire))
        return;
    send_recovery_ready_flag.store (true, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_ready ()
{
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_pending () const
{
    return send_recovery_pending_flag.load (std::memory_order_acquire);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_ready () const
{
    return send_recovery_ready_flag.load (std::memory_order_acquire);
}
