/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>

namespace zlink::framework::detail
{

template <typename SocketT>
void apply_common_channel_socket_options (SocketT &socket,
                                          const channel_capability_snapshot_t &capability)
{
    auto options = socket.options ();
    if (capability.send_high_water_mark) {
        options.send_hwm (*capability.send_high_water_mark);
    }
    if (capability.receive_high_water_mark) {
        options.recv_hwm (*capability.receive_high_water_mark);
    }
    if (capability.max_message_size) {
        options.max_message_size (*capability.max_message_size);
    }
}

template <typename SocketT>
void apply_weighted_channel_socket_options (SocketT &socket,
                                            const channel_capability_snapshot_t &capability)
{
    apply_common_channel_socket_options (socket, capability);
    if (capability.peer_weight) {
        socket.options ().peer_weight (*capability.peer_weight);
    }
}

} // namespace zlink::framework::detail
