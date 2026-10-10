/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>

namespace zlink::framework::detail
{

inline std::chrono::milliseconds
channel_send_timeout (std::optional<std::chrono::milliseconds> timeout)
{
    return timeout.value_or (default_fanout_send_timeout);
}

template <typename SocketT>
void apply_channel_send_timeout (SocketT &socket, std::optional<std::chrono::milliseconds> timeout)
{
    socket.options ().send_timeout (channel_send_timeout (timeout));
}

template <typename SocketT>
void apply_common_channel_socket_options (SocketT &socket,
                                          const channel_capability_snapshot_t &capability)
{
    auto options = socket.options ();
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

template <typename SocketT>
void apply_fanout_publisher_socket_options (SocketT &socket,
                                            bool no_drop,
                                            std::optional<std::chrono::milliseconds> send_timeout)
{
    socket.options ().linger (std::chrono::milliseconds::zero ());
    apply_channel_send_timeout (socket, send_timeout);
    socket.options ().no_drop (no_drop);
}

} // namespace zlink::framework::detail
