/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_pending_requests.hpp"
#include "runtime/channels/route_connection_set.hpp"
#include "runtime/messaging/client_call_codec.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

struct route_outbound_packet_t
{
    zlink::routing_id_t target_node_rid;
    std::optional<std::string> target_spot_id;
    runtime::messaging::message_parts_t parts;
    std::optional<std::uint64_t> request_seq;
};

class route_channel_runtime_t
{
  public:
    using send_backend_t =
      std::function<result_t<void> (const zlink::routing_id_t &,
                                    const std::optional<std::string> &,
                                    const runtime::messaging::message_parts_t &)>;
    using request_backend_t = std::function<result_t<runtime::messaging::message_parts_t> (
      const zlink::routing_id_t &,
      const std::optional<std::string> &,
      const runtime::messaging::message_parts_t &,
      std::chrono::milliseconds)>;

    explicit route_channel_runtime_t (std::string router_channel_id);

    const std::string &router_channel_id () const noexcept;
    void routing_id (zlink::routing_id_t routing_id);
    const std::optional<zlink::routing_id_t> &routing_id () const noexcept;
    void default_request_timeout (std::chrono::milliseconds timeout);
    std::chrono::milliseconds default_request_timeout () const noexcept;
    void start () noexcept;
    void stop () noexcept;
    bool running () const noexcept;

    bool connect (std::string endpoint);
    bool connect (zlink::routing_id_t peer_rid, std::string endpoint);
    bool disconnect (const std::string &endpoint);
    std::vector<std::string> list_connections () const;
    std::vector<route_connection_set_t::target_t> list_connection_targets () const;
    void mark_peer_ready (const zlink::routing_id_t &peer_rid);
    void mark_peer_disconnected (const zlink::routing_id_t &peer_rid);
    void bind_endpoint (std::string endpoint);
    const std::string &bind_endpoint () const noexcept;
    void manual_connections (std::vector<std::string> endpoints);
    std::vector<std::string> manual_connections () const;

    template <typename TMessage>
    result_t<void> submit_send (const zlink::routing_id_t &target_node_rid,
                                std::string packet_name,
                                const TMessage &message,
                                const serializer_registry_t &serializers)
    {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             _router_channel_id, std::move (packet_name));
        return submit_send_parts (target_node_rid,
                                  codec.encode_envelope_parts (header, message, serializers));
    }

    template <typename TRequest>
    result_t<std::uint64_t> submit_request (const zlink::routing_id_t &target_node_rid,
                                            std::string packet_name,
                                            const TRequest &request,
                                            const serializer_registry_t &serializers,
                                            std::chrono::milliseconds timeout)
    {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             _router_channel_id, std::move (packet_name), timeout);
        return submit_request_parts (target_node_rid,
                                     codec.encode_envelope_parts (header, request, serializers));
    }

    result_t<void> submit_send_parts (const zlink::routing_id_t &target_node_rid,
                                      runtime::messaging::message_parts_t parts);

    result_t<std::uint64_t> submit_request_parts (const zlink::routing_id_t &target_node_rid,
                                                  runtime::messaging::message_parts_t parts);

    result_t<runtime::messaging::message_parts_t>
    request_reply_parts (const zlink::routing_id_t &target_node_rid,
                         runtime::messaging::message_parts_t parts,
                         std::chrono::milliseconds timeout);

    result_t<void> submit_spot_send_parts (const zlink::routing_id_t &target_node_rid,
                                           const std::string &target_spot_id,
                                           runtime::messaging::message_parts_t parts);

    result_t<std::uint64_t> request_to_spot_parts (const zlink::routing_id_t &target_node_rid,
                                                   const std::string &target_spot_id,
                                                   runtime::messaging::message_parts_t parts);

    result_t<runtime::messaging::message_parts_t>
    request_reply_spot_parts (const zlink::routing_id_t &target_node_rid,
                              const std::string &target_spot_id,
                              runtime::messaging::message_parts_t parts,
                              std::chrono::milliseconds timeout);

    result_t<void> complete_request (std::uint64_t request_seq);
    void set_send_backend (send_backend_t backend);
    void set_request_backend (request_backend_t backend);
    const std::vector<route_outbound_packet_t> &outbound_packets () const noexcept;
    std::size_t pending_request_count () const noexcept;
    result_t<void> wait_until_connected (std::chrono::milliseconds timeout) const;
    result_t<void> wait_until_peer_ready (const zlink::routing_id_t &target_node_rid,
                                          std::chrono::milliseconds timeout) const;

  private:
    route_outbound_packet_t &
    append_outbound_unlocked (const zlink::routing_id_t &target_node_rid,
                              std::optional<std::string> target_spot_id,
                              runtime::messaging::message_parts_t parts,
                              std::optional<std::uint64_t> request_seq);
    result_t<std::uint64_t>
    register_request_unlocked (const zlink::routing_id_t &target_node_rid,
                               std::optional<std::string> target_spot_id,
                               runtime::messaging::message_parts_t parts);
    result_t<void> ensure_connected () const;
    std::string _router_channel_id;
    mutable std::mutex _mutex;
    std::optional<zlink::routing_id_t> _routing_id;
    std::chrono::milliseconds _default_request_timeout{std::chrono::seconds (30)};
    std::string _bind_endpoint;
    std::vector<std::string> _manual_connections;
    bool _running = false;
    route_connection_set_t _connections;
    std::set<std::string> _ready_peer_rids;
    channel_pending_requests_t _pending_requests;
    std::vector<route_outbound_packet_t> _outbound_packets;
    send_backend_t _send_backend;
    request_backend_t _request_backend;
    std::mutex _send_backend_mutex;
};

} // namespace zlink::framework::detail
