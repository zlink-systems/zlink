/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/config_result_internal.hpp"
#include "api/core/zlink_option_internal.hpp"

//  Public surface for core-byte-hwm-flow-control-plan.ko.md §5/§7. This is
//  the completion boundary the plan describes: it returns once the
//  socket-owning runtime thread has stored the local state (and queued
//  fanout to the current transport pairs and future ones), not once a remote
//  peer has observed it.
extern "C" zlink_config_result_t
zlink_socket_set_receive_flow_state (void *handle_,
                                     zlink_receive_flow_state_t state_)
{
    zlink::socket_base_t *socket = as_socket (handle_);
    if (!socket)
        return ZLINK_CONFIG_INVALID_HANDLE;

    return zlink::config_result_internal::from_rc (
      socket->set_local_receive_flow_state (static_cast<int> (state_)));
}
