/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

#include "utils/config.hpp"

bool zlink::socket_command_runtime_t::should_skip_throttled_command_poll (uint64_t tsc_)
{
    if (!tsc_)
        return false;

    if (tsc_ >= last_command_tsc && tsc_ - last_command_tsc <= max_command_delay)
        return true;

    last_command_tsc = tsc_;
    return false;
}

bool zlink::socket_command_runtime_t::should_poll_commands_after_recv (int inbound_poll_rate_)
{
    return ++recv_ticks == inbound_poll_rate_;
}

void zlink::socket_command_runtime_t::reset_recv_ticks ()
{
    recv_ticks = 0;
}

bool zlink::socket_command_runtime_t::should_block_on_recv () const
{
    return recv_ticks != 0;
}
