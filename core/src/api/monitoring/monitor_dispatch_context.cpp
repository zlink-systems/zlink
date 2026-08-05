/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/monitor_api_internal.hpp"

namespace
{
monitor_handler_state_t *&monitor_handler_state_tls ()
{
    static thread_local monitor_handler_state_t *state = NULL;
    return state;
}
}

zlink::monitor_dispatch_context_t::monitor_dispatch_context_t (monitor_handler_state_t *state_) :
    _previous_state (monitor_handler_state_tls ())
{
    monitor_handler_state_tls () = state_;
}

zlink::monitor_dispatch_context_t::~monitor_dispatch_context_t ()
{
    monitor_handler_state_tls () = _previous_state;
}

monitor_handler_state_t *zlink::monitor_dispatch_context_t::current_state ()
{
    return monitor_handler_state_tls ();
}

void *zlink::monitor_dispatch_context_t::current_handle ()
{
    monitor_handler_state_t *state = monitor_handler_state_tls ();
    return state ? static_cast<void *> (state->socket) : NULL;
}

monitor_handler_state_t *zlink::current_monitor_handler_state ()
{
    return monitor_dispatch_context_t::current_state ();
}

void *zlink::current_monitor_dispatch_handle ()
{
    return monitor_dispatch_context_t::current_handle ();
}
