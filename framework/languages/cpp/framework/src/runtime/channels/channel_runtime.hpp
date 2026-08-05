/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

#include "runtime/channels/channel_pending_requests.hpp"
#include "runtime/channels/channel_runtime_bundle.hpp"
#include "runtime/channels/route_channel_registration.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace zlink::framework::detail
{

struct mesh_node_builder_state_t;
class channel_runtime_state_t;
class monitoring_runtime_state_t;
class stream_runtime_state_t;
class channel_native_client_t;
class channel_native_publisher_t;

} // namespace zlink::framework::detail

namespace zlink::framework::runtime
{
class offload_executor_t;
class spot_address_resolver_t;
} // namespace zlink::framework::runtime

namespace zlink::framework::detail
{

class route_client_runtime_t
{
  public:
    template <typename TRequest, typename TReply>
    static task_t<TReply> request_to_spot_address (
      const route_client_t &client,
      runtime::spot_address_t address,
      TRequest request,
      std::chrono::milliseconds timeout)
    {
        if (client._serializers == nullptr) {
            co_return result_t<TReply>::failure (
              framework_error_kind_t::protocol_error,
              "route client has no serializer registry");
        }
        auto request_value = std::make_shared<TRequest> (std::move (request));
        auto reply = co_await route_client_t::submit_spot_request_reply_message_erased (
          client._state, std::move (address.mesh_name), std::move (address.node_rid),
          std::move (address.spot_id), address.spot_generation,
          detail::message_name<TRequest> (), std::type_index (typeid (TRequest)),
          [request_value] (serializer_registry_t &serializers) {
              return serializers.template get<TRequest> ().serialize (*request_value);
          },
          timeout, {});
        try {
            co_return result_t<TReply>::success (
              client._serializers->template get<TReply> ().deserialize (
                detail::encoded_payload_from_raw (reply)));
        }
        catch (const framework_exception_t &error) {
            co_return detail::result_access_t::failure<TReply> (error);
        }
    }
};

result_t<void> validate_channel_native_reply (const runtime::messaging::message_parts_t &parts);

class capability_builder_state_t
{
  public:
    channel_capability_snapshot_t *target = nullptr;
    channel_capability_snapshot_t snapshot;
};

class channel_builder_state_t
{
  public:
    explicit channel_builder_state_t (std::string name) : snapshot{std::move (name)} {}

    channel_snapshot_t *target = nullptr;
    channel_snapshot_t snapshot;
};

class route_channel_builder_state_t
{
  public:
    explicit route_channel_builder_state_t (std::string router_channel_id) :
        registration (std::move (router_channel_id))
    {
    }

    route_channel_registration_t registration;
};

class route_client_state_t
{
  public:
    route_client_state_t (std::shared_ptr<channel_runtime_state_t> runtime,
                          serializer_registry_t &serializers);
    ~route_client_state_t ();

    std::shared_ptr<channel_runtime_state_t> runtime;
    serializer_registry_t *serializers;
    std::shared_ptr<runtime::offload_executor_t> executor;
};

class channel_runtime_state_t
{
  public:
    runtime::spot_address_resolver_t *spot_resolver = nullptr;
    using instance_spot_send_t = std::function<result_t<void> (
      const spot_id_t &, const spot_activation_intent_t &,
      const std::string &, std::type_index,
      std::function<encoded_payload_t (serializer_registry_t &)>,
      const std::map<std::string, std::string> &)>;
    using instance_spot_request_t = std::function<task_t<zlink::message_t> (
      const spot_id_t &, const spot_activation_intent_t &,
      std::string, std::type_index,
      std::function<encoded_payload_t (serializer_registry_t &)>,
      std::chrono::milliseconds,
      std::map<std::string, std::string>)>;
    using mesh_node_send_t = std::function<result_t<void> (
      const zlink::routing_id_t &,
      runtime::messaging::message_parts_t)>;
    using mesh_node_request_t = std::function<result_t<runtime::messaging::message_parts_t> (
      const zlink::routing_id_t &,
      runtime::messaging::message_parts_t,
      std::chrono::milliseconds)>;
    using mesh_channel_send_t = std::function<result_t<void> (
      runtime::messaging::message_parts_t)>;
    using mesh_channel_request_t = std::function<result_t<runtime::messaging::message_parts_t> (
      runtime::messaging::message_parts_t,
      std::chrono::milliseconds)>;
    using client_server_send_t = std::function<result_t<void> (
      std::string,
      std::string,
      zlink::message_t,
      std::chrono::milliseconds)>;
    using client_server_request_t = std::function<result_t<zlink::message_t> (
      std::string,
      std::string,
      zlink::message_t,
      std::chrono::milliseconds)>;
    using fanout_publish_t = std::function<result_t<void> (
      std::string,
      std::string,
      std::string,
      zlink::message_t,
      std::chrono::milliseconds)>;
    using spot_mesh_send_t = std::function<result_t<void> (
      const zlink::routing_id_t &,
      const std::string &,
      std::uint64_t,
      runtime::messaging::message_parts_t)>;
    using spot_mesh_request_t = std::function<result_t<runtime::messaging::message_parts_t> (
      const zlink::routing_id_t &,
      const std::string &,
      std::uint64_t,
      runtime::messaging::message_parts_t,
      std::chrono::milliseconds)>;

    struct outbound_call_record_t
    {
        std::string kind;
        std::string channel_name;
        std::string topic;
        std::string packet_name;
        std::chrono::milliseconds timeout{0};
        std::map<std::string, std::string> metadata;
    };

    std::map<std::string, channel_snapshot_t> channels;
    mutable std::mutex mutex;
    std::size_t max_pending = 1024;
    std::chrono::milliseconds default_request_timeout{std::chrono::seconds (30)};
    std::size_t pending = 0;
    channel_pending_requests_t pending_requests;
    std::map<std::string, std::shared_ptr<channel_runtime_bundle_t>> server_bundles;
    std::map<std::string, std::shared_ptr<channel_runtime_bundle_t>> client_bundles;
    std::map<std::string, std::shared_ptr<channel_runtime_bundle_t>> publisher_bundles;
    std::map<std::string, std::shared_ptr<channel_runtime_bundle_t>> subscriber_bundles;
    std::map<std::string, std::shared_ptr<channel_native_client_t>> native_clients;
    std::vector<std::weak_ptr<channel_native_client_t>> native_request_clients;
    std::map<std::string, std::shared_ptr<channel_native_publisher_t>> native_publishers;
    std::map<std::string, std::shared_ptr<route_channel_runtime_t>> route_channels;
    std::map<std::string, mesh_node_send_t> mesh_node_senders;
    std::map<std::string, mesh_node_request_t> mesh_node_requesters;
    std::map<std::string, mesh_channel_send_t> mesh_channel_senders;
    std::map<std::string, mesh_channel_request_t> mesh_channel_requesters;
    std::map<std::string, client_server_send_t> client_server_senders;
    std::map<std::string, client_server_request_t> client_server_requesters;
    std::map<std::string, fanout_publish_t> fanout_publishers;
    std::map<std::string, spot_mesh_send_t> spot_mesh_senders;
    std::map<std::string, spot_mesh_request_t> spot_mesh_requesters;
    instance_spot_send_t instance_spot_sender;
    instance_spot_request_t instance_spot_requester;
    std::vector<std::weak_ptr<runtime::offload_executor_t>> route_client_executors;
    std::map<std::string, route_handler_registry_t> route_handlers;
    std::map<std::string, int> server_peer_weight_overrides;
    std::map<std::string, std::uint64_t> weighted_discovery_cursors;
    std::map<std::string, std::string> last_discovery_request_endpoints;
    std::vector<outbound_call_record_t> outbound_calls;
    dispatch_options_t dispatch;
    serializer_registry_t *serializers = nullptr;
    std::shared_ptr<monitoring_runtime_state_t> monitoring;
    bool auto_connect_active = false;
    bool shutdown = false;
    bool closed = false;
};

class zlink_builder_state_t
{
  public:
    ~zlink_builder_state_t ();

    std::string node_name;
    std::shared_ptr<channel_runtime_state_t> runtime = std::make_shared<channel_runtime_state_t> ();
    std::map<std::string, std::shared_ptr<mesh_node_builder_state_t>> mesh_nodes;
    std::map<std::string, std::shared_ptr<route_channel_builder_state_t>> route_channels;
    std::shared_ptr<stream_runtime_state_t> stream_runtime =
      std::make_shared<stream_runtime_state_t> ();
};

class channel_runtime_t
{
  public:
    explicit channel_runtime_t (std::shared_ptr<channel_runtime_state_t> state);

    std::vector<channel_snapshot_t> channel_snapshots () const;

    result_t<zlink::message_t> dispatch_request (std::string channel_name,
                                                 std::string topic,
                                                 std::string packet_name,
                                                 service_provider_t &services,
                                                 serializer_registry_t &serializers,
                                                 const handler_registry_t &handlers,
                                                 const zlink::message_t &message,
                                                 const detail::inbound_message_context_t
                                                   &inbound = {}) const;

    result_t<void> dispatch_send (std::string channel_name,
                                  std::string topic,
                                  std::string packet_name,
                                  service_provider_t &services,
                                  serializer_registry_t &serializers,
                                  const handler_registry_t &handlers,
                                  const zlink::message_t &message,
                                  const detail::inbound_message_context_t &inbound = {}) const;

    result_t<std::uint64_t> reserve_outbound_request (std::string channel_name);
    result_t<void> complete_outbound_reply (std::uint64_t request_seq);
    result_t<void> cancel_outbound_request (std::uint64_t request_seq);
    void close () noexcept;
    void shutdown () noexcept;
    /* Live manual endpoint mutation for a client channel (endpoint
     * connections contract): requests iterate the same bundle set. */
    void add_client_manual_connection (const std::string &channel_name,
                                       const std::string &endpoint);
    void remove_client_manual_connection (const std::string &channel_name,
                                          const std::string &endpoint);
    void add_subscriber_manual_connection (const std::string &channel_name,
                                           const std::string &endpoint);
    void remove_subscriber_manual_connection (const std::string &channel_name,
                                              const std::string &endpoint);
    std::size_t pending_count () const noexcept;
    std::size_t pending_limit () const noexcept;
    std::vector<channel_runtime_state_t::outbound_call_record_t> outbound_calls () const;
    void bind_serializers (serializer_registry_t &serializers) noexcept;
    void bind_spot_mesh_transport (std::string mesh_name,
                                   channel_runtime_state_t::spot_mesh_send_t send,
                                   channel_runtime_state_t::spot_mesh_request_t request);
    void bind_spot_address_resolver (runtime::spot_address_resolver_t &resolver) noexcept;
    void bind_instance_spot_activator (
      channel_runtime_state_t::instance_spot_send_t send,
      channel_runtime_state_t::instance_spot_request_t request);
    void bind_mesh_node_transport (std::string mesh_name,
                                   channel_runtime_state_t::mesh_node_send_t send,
                                   channel_runtime_state_t::mesh_node_request_t request);
    void bind_mesh_channel_transport (
      std::string channel_name,
      channel_runtime_state_t::mesh_channel_send_t send,
      channel_runtime_state_t::mesh_channel_request_t request);
    void bind_client_server_transport (
      std::string channel_name,
      channel_runtime_state_t::client_server_send_t send,
      channel_runtime_state_t::client_server_request_t request);
    void unbind_client_server_transport (const std::string &channel_name) noexcept;
    void bind_fanout_transport (
      std::string channel_name,
      channel_runtime_state_t::fanout_publish_t publish);
    void unbind_fanout_transport (
      const std::string &channel_name) noexcept;
    dispatch_options_t dispatch_options () const;
    const dispatch_options_t &dispatch_options_ref () const noexcept { return _state->dispatch; }
    void mark_auto_connect_active ();
    bool auto_connect_active () const;
    void drain () noexcept;
    void publish_socket_event (const std::string &channel_name,
                               socket_event_kind_t event,
                               std::string local_address = {},
                               std::string remote_address = {}) const;
    void set_server_weight (const std::string &channel_name, int value);
    std::optional<int>
    server_peer_weight_override (const std::string &channel_name) const;

    static channel_runtime_t from (const message_bus_t &bus);

  private:
    std::shared_ptr<channel_runtime_state_t> _state;
};

} // namespace zlink::framework::detail
