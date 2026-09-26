/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_node_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/stream_connector.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
using namespace std::chrono_literals;

template <typename T>
concept exposes_descriptor_revision = requires (const T &value) { value.descriptor_revision; };

template <typename T>
concept exposes_endpoint = requires (const T &value) { value.endpoint; };

static_assert (!exposes_descriptor_revision<zlink::framework::mesh_node_snapshot_t>);
static_assert (!exposes_endpoint<zlink::framework::mesh_node_snapshot_t>);
static_assert (!exposes_descriptor_revision<zlink::framework::mesh_peer_snapshot_t>);
static_assert (!exposes_endpoint<zlink::framework::mesh_peer_snapshot_t>);

class monitoring_mesh_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    void set_local (zlink::framework::mesh_node_descriptor_t descriptor)
    {
        std::lock_guard lock (_mutex);
        _descriptor = std::move (descriptor);
    }

    zlink::framework::mesh_node_descriptor_t local () const
    {
        std::lock_guard lock (_mutex);
        return _descriptor;
    }

    void set_remote (std::optional<zlink::framework::mesh_node_descriptor_t> descriptor)
    {
        std::lock_guard lock (_mutex);
        _remote = std::move (descriptor);
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t value,
                      zlink::framework::location_write_intent_t) override
    {
        std::lock_guard lock (_mutex);
        _descriptor = std::move (value);
        return zlink::framework::task_t<zlink::framework::location_write_result_t> (
          zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
            zlink::framework::location_write_result_t::stored (1, {})));
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_mesh_node (zlink::framework::mesh_node_descriptor_key_t,
                      zlink::framework::location_owner_token_t) override
    {
        return zlink::framework::task_t<zlink::framework::location_write_status_t> (
          zlink::framework::result_t<zlink::framework::location_write_status_t>::success (
            zlink::framework::location_write_status_t::stored));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, zlink::framework::location_page_request_t = {}) override
    {
        std::lock_guard lock (_mutex);
        zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t> page;
        if (_descriptor.mesh_name == mesh_name)
            page.items.push_back (_descriptor);
        if (_remote && _remote->mesh_name == mesh_name)
            page.items.push_back (*_remote);
        return zlink::framework::task_t<decltype (page)> (
          zlink::framework::result_t<decltype (page)>::success (std::move (page)));
    }

  private:
    mutable std::mutex _mutex;
    zlink::framework::mesh_node_descriptor_t _descriptor;
    std::optional<zlink::framework::mesh_node_descriptor_t> _remote;
};

class faulting_mesh_location_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    void fail_next_retiring_write (std::size_t ordinal)
    {
        std::lock_guard lock (_fault_gate);
        _retiring_write = 0;
        _fail_retiring_write = ordinal;
    }

    std::vector<zlink::framework::framework_runtime_state_t> state_history () const
    {
        std::lock_guard lock (_fault_gate);
        return _state_history;
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t descriptor,
                      zlink::framework::location_write_intent_t intent) override
    {
        {
            std::lock_guard lock (_fault_gate);
            _state_history.push_back (descriptor.state);
            if (descriptor.state == zlink::framework::framework_runtime_state_t::relocating
                && ++_retiring_write == _fail_retiring_write) {
                _fail_retiring_write = 0;
                return zlink::framework::task_t<zlink::framework::location_write_result_t> (
                  zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
                    {zlink::framework::location_write_status_t::rejected_conflict, 0, {}}));
            }
        }
        return in_memory_location_repository_t::update_mesh_node (std::move (descriptor), intent);
    }

  private:
    mutable std::mutex _fault_gate;
    std::size_t _retiring_write = 0;
    std::size_t _fail_retiring_write = 0;
    std::vector<zlink::framework::framework_runtime_state_t> _state_history;
};

static_assert (std::is_same_v<decltype (std::declval<zlink::framework::zlink_builder_t &> ()
                                          .add_route_mesh (std::declval<std::string> ())),
                              zlink::framework::mesh_node_builder_t>);
static_assert (std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                                          .channel_name (std::declval<std::string> ())),
                              zlink::framework::mesh_channel_builder_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_channel_builder_t &> ().client ()),
                 zlink::framework::mesh_channel_client_builder_t>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_channel_builder_t &> ().server ()),
                 zlink::framework::mesh_channel_server_builder_t>);
struct contract_actor_t final : zlink::framework::actor_t
{
    explicit contract_actor_t (zlink::framework::actor_context_t context) :
        value (std::move (context))
    {
    }
    zlink::framework::actor_context_t &context () noexcept override { return value; }
    const zlink::framework::actor_context_t &context () const noexcept override { return value; }
    zlink::framework::actor_context_t value;
};
struct contract_actor_factory_t final : zlink::framework::actor_factory_t<contract_actor_t>
{
    zlink::framework::task_t<std::shared_ptr<contract_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<contract_actor_t> (std::move (context));
    }
};
struct contract_entry_spot_t : zlink::framework::entry_spot_t<contract_actor_t>
{
};
struct contract_spot_t : zlink::framework::spot_t<contract_actor_t>
{
};
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                             .add_entry_spot<contract_entry_spot_t> (
                               std::declval<std::function<std::shared_ptr<contract_entry_spot_t> (
                                 zlink::framework::entry_spot_context_t)>> ())),
                 zlink::framework::mesh_node_builder_t &>);
static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                .add_spot_factory<contract_spot_t> (
                  std::declval<std::string> (),
                  std::declval<std::function<
                    std::shared_ptr<contract_spot_t> (zlink::framework::spot_context_t)>> (),
                  std::declval<std::function<
                    void (zlink::framework::user_spot_factory_builder_t<contract_spot_t> &)>> ())),
    zlink::framework::mesh_node_builder_t &>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::mesh_node_builder_t &> ()
                             .add_actor_factory<contract_actor_t, contract_actor_factory_t> (
                               std::declval<std::string> (),
                               std::declval<std::shared_ptr<contract_actor_factory_t>> (),
                               [] (auto &factory) { factory.disable_relocation (); })),
                 zlink::framework::mesh_node_builder_t &>);
bool wait_until_admitted (zlink::framework::detail::mesh_node_runtime_t &node)
{
    // The transport drains its socket monitor from dispatch_ready, so a waiter
    // pumps the node the way the host service does instead of sleeping blind.
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (node.admitted_peer_count () > 0)
            return true;
        (void) std::move (
          node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                   const zlink::framework::runtime::host::receive_record_t &,
                                   std::vector<zlink::message_t>) {}))
          .result ()
          .value ();
        std::this_thread::sleep_for (10ms);
    }
    std::fprintf (stderr, "[vertical] admission timeout rid=%s peers=%zu state=%d\n",
                  node.status ().routing_id ().to_string ().c_str (), node.admitted_peer_count (),
                  static_cast<int> (node.status ().state));
    return false;
}

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_node (std::string endpoint, std::string routing_id);

void register_mesh_location_resolvers (zlink::framework::service_collection_t &services);

void verify_object_client_registration_boundary ()
{
    auto client_state = make_node ("tcp://127.0.0.1:*", "object-client");
    client_state->object_role = zlink::framework::object_role_t::client;
    client_state->channels.clear ();
    zlink::framework::detail::mesh_node_runtime_t client (client_state);
    client.start ();
    const auto descriptor = client.native_node ().transport ().topology ().local_descriptor ();
    assert (descriptor.object_role
            == zlink::framework::runtime::mesh::service_object_role_t::client);
    assert (descriptor.channels.empty ());
    client.stop ();

    auto invalid_state = make_node ("tcp://127.0.0.1:*", "invalid-object-client");
    invalid_state->object_role = zlink::framework::object_role_t::client;
    invalid_state->has_node_direct_handler = true;
    zlink::framework::detail::mesh_node_runtime_t invalid (invalid_state);
    bool rejected = false;
    try {
        invalid.start ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected = true;
    }
    assert (rejected);
}

bool wait_until_admitted_count (zlink::framework::detail::mesh_node_runtime_t &node,
                                std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (node.admitted_peer_count () >= expected)
            return true;
        (void) std::move (
          node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                   const zlink::framework::runtime::host::receive_record_t &,
                                   std::vector<zlink::message_t>) {}))
          .result ()
          .value ();
        std::this_thread::sleep_for (10ms);
    }
    return false;
}

template <typename TSubmit> bool submit_until_ok (TSubmit submit)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    zlink::submit_result_t last = zlink::submit_result_t::internal_error;
    while (std::chrono::steady_clock::now () < deadline) {
        last = std::move (submit ()).result ().value ();
        if (last == zlink::submit_result_t::ok)
            return true;
        std::this_thread::sleep_for (10ms);
    }
    std::cerr << "last submit result=" << static_cast<int> (last) << '\n';
    return false;
}

void configure_vertical_route_fences (zlink::framework::detail::mesh_node_runtime_t &node)
{
    // This vertical test exercises the raw MeshNode transport without the
    // application Location Store. Supply the same non-zero owner fence that
    // the target objects receive from the minimal stateful runtime.
    node.configure_spot_route_fence_resolver (
      [] (const zlink::routing_id_t &, std::string_view,
          std::uint64_t) -> std::optional<zlink::framework::runtime::host::route_fence_t> {
          return zlink::framework::runtime::host::route_fence_t{1, 1};
      },
      std::chrono::minutes (1));
}

bool receive_one (zlink::framework::detail::mesh_node_runtime_t &node,
                  zlink::framework::runtime::host::record_kind_t expected_kind,
                  const std::string &expected_text,
                  const std::vector<std::uint8_t> &expected_metadata)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        bool matched = false;
        (void) std::move (
          node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &,
                                    const zlink::framework::runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts) {
              matched = matched
                        || (record.kind == expected_kind && !parts.empty ()
                            && parts.front ().to_string () == expected_text);
          }))
          .result ()
          .value ();
        if (matched)
            return true;
        std::this_thread::sleep_for (5ms);
    }
    return false;
}

bool reply_to_one_request (zlink::framework::detail::mesh_node_runtime_t &node,
                           zlink::framework::runtime::host::record_kind_t expected_kind,
                           const std::string &expected_text,
                           const std::string &reply_text)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        bool replied = false;
        (void) std::move (
          node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &,
                                    const zlink::framework::runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts) {
              if (record.kind == expected_kind && !parts.empty ()
                  && parts.front ().to_string () == expected_text) {
                  const std::vector<zlink::message_t> reply_parts{
                    zlink::message_t::from (reply_text)};
                  replied = zlink::framework::runtime::host::reply (record.reply_token, reply_parts)
                            == zlink::submit_result_t::ok;
              }
          }))
          .result ()
          .value ();
        if (replied)
            return true;
        std::this_thread::sleep_for (5ms);
    }
    return false;
}

bool receive_completion (zlink::framework::detail::mesh_node_runtime_t &node,
                         const zlink::framework::runtime::host::pending_operation_t &operation_id,
                         const std::string &expected_text)
{
    auto completion = node.await_completion (operation_id);
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!completion.await_ready () && std::chrono::steady_clock::now () < deadline) {
        // Only the binding receive owner progresses. No host dispatch turn
        // is needed between binding terminal and the registered result.
        (void) node.native_node ()
          .transport ()
          .pump_one (std::chrono::steady_clock::now (), false)
          .result ()
          .value ();
        std::this_thread::yield ();
    }
    if (!completion.await_ready () || !completion.result ())
        return false;
    const auto &settled = completion.result ().value ();
    return settled.record.operation_id == operation_id.id && settled.record.terminal_result == 0
           && !settled.parts.empty () && settled.parts.front ().to_string () == expected_text;
}

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_node (std::string endpoint, std::string routing_id)
{
    auto state =
      std::make_shared<zlink::framework::detail::mesh_node_builder_state_t> ("vertical-mesh");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = std::move (endpoint);
    state->listen_port.reset ();
    state->routing_id = zlink::routing_id_t::from (routing_id);
    state->object_role = zlink::framework::object_role_t::server;
    state->channels.emplace (
      "work", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    // The host admits object creation only for declared stable types.
    state->spot_state->snapshot.actor_types.emplace_back ("vertical.actor");
    return state;
}

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_named_node (std::string mesh_name, std::string routing_id)
{
    auto state =
      std::make_shared<zlink::framework::detail::mesh_node_builder_state_t> (std::move (mesh_name));
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from (std::move (routing_id));
    state->object_role = zlink::framework::object_role_t::server;
    state->channels.emplace (
      "work", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    // The host admits object creation only for declared stable types.
    state->spot_state->snapshot.actor_types.emplace_back ("vertical.actor");
    return state;
}

void verify_unselected_object_role_defaults_to_none ()
{
    auto state = std::make_shared<zlink::framework::detail::mesh_node_builder_state_t> (
      "unselected-object-role");
    zlink::framework::detail::mesh_node_runtime_t runtime (state);

    assert (state->object_role == zlink::framework::object_role_t::none);
    assert (runtime.object_role () == zlink::framework::object_role_t::none);
    assert (state->spot_state->spot_factories.empty ());
    assert (!state->spot_state->snapshot.entry_spot_name.has_value ());
}

zlink::message_t make_route_multicast_frame (std::string_view packet_name,
                                             std::string_view topic,
                                             std::string_view payload)
{
    namespace messaging = zlink::framework::runtime::messaging;
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::publish;
    header.channel_name = "vertical-mesh";
    header.message_name = packet_name;
    header.content_type = messaging::envelope_codec_t::default_content_type;
    header.topic = std::string (topic);
    header.source = header.channel_name;
    const auto header_bytes = messaging::envelope_codec_t{}.encode_header (header).to_bytes ();
    std::vector<std::uint8_t> frame{'Z', 'L', 'F', 'E'};
    const auto header_size = static_cast<std::uint32_t> (header_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> (header_size >> 24));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 16));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 8));
    frame.push_back (static_cast<std::uint8_t> (header_size));
    frame.insert (frame.end (), header_bytes.begin (), header_bytes.end ());
    frame.insert (frame.end (), payload.begin (), payload.end ());
    return zlink::message_t::from (std::move (frame));
}

void enqueue_application_route (
  const std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t> &node,
  zlink::framework::runtime::host::ready_record_t owner,
  zlink::framework::runtime::host::receive_record_t record,
  std::vector<zlink::message_t> parts)
{
    namespace mesh = zlink::framework::runtime::mesh;
    mesh::service_mailbox_record_t mailbox_record;
    mailbox_record.owner = mesh::service_mailbox_t::application_owner (owner);
    mailbox_record.domain = mesh::service_mailbox_domain_t::application;
    mailbox_record.before_application_handler = [] {};
    mailbox_record.application =
      std::make_shared<zlink::framework::runtime::host::local_application_dispatch_t> (
        zlink::framework::runtime::host::local_application_dispatch_t{
          std::move (owner), std::move (record), std::move (parts)});
    assert (node->native_node ().transport ().mailbox ().try_enqueue (std::move (mailbox_record)));
}

void verify_route_internal_packets_precede_application_dispatch ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    auto registration = make_node ("tcp://127.0.0.1:0", "internal-route-owner");
    auto worker = std::make_shared<runtime::offload_executor_t> (1, "rel-358-internal-route");
    registration->spot_state->worker_executor = worker;
    spot_node_runtime_t spots (registration->spot_state);
    spots.set_route_client (route_client_t{});

    auto context = std::make_shared<spot_context_state_t> ();
    context->node = registration->spot_state;
    context->lane_owner = registration->spot_state;
    context->node_rid = node_rid_t::from_string ("internal-route-owner");
    context->mesh_name = "vertical-mesh";
    context->spot_id = spot_id_t ("internal-route-spot");
    context->spot_name = "internal-route-spot";
    context->spot_instance = std::make_shared<int> (1);
    context->serial_executor = worker;

    auto multicast_received = std::make_shared<std::promise<std::string>> ();
    auto multicast_future = multicast_received->get_future ();
    context->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::subscription, "Rel358MulticastPayload", "rel-358-topic",
      std::type_index (typeid (int)), std::type_index (typeid (void)),
      std::type_index (typeid (void)), std::type_index (typeid (void))});
    context->handler_invokers.push_back (
      [multicast_received] (void *, void *, service_provider_t &, serializer_registry_t &,
                            const zlink::message_t &message,
                            const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          multicast_received->set_value (message.to_string ());
          co_return zlink::message_t{};
      });

    std::atomic_bool disconnect_called{false};
    spot_actor_admission_callbacks_t actor_callbacks;
    actor_callbacks.on_disconnect_actor = [&disconnect_called] (void *, void *) -> task_t<void> {
        disconnect_called.store (true, std::memory_order_release);
        co_return;
    };
    context->actor_admissions.emplace (std::type_index (typeid (int)), std::move (actor_callbacks));
    registration->spot_state->spot_contexts_by_id.emplace (context->spot_id,
                                                           spot_context_access_t::create (context));

    spot_node_builder_state_t::actor_factory_registration_t actor_factory;
    actor_factory.actor_type = std::type_index (typeid (int));
    actor_factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    actor_factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    registration->spot_state->actor_factories.emplace ("vertical.actor", std::move (actor_factory));
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("internal-route-owner"),
                                                 "vertical.actor", "internal-route-actor", 1);
    registration->spot_state->actor_instances.emplace ("vertical.actor:internal-route-actor",
                                                       std::make_shared<int> (7));
    registration->spot_state->actor_spot_ids.emplace ("vertical.actor:internal-route-actor",
                                                      context->spot_id);
    registration->spot_state->actor_generations.emplace ("vertical.actor:internal-route-actor", 1);

    serializer_registry_t serializers;
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (location_store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    auto &location_runtime = provider.get_required<runtime::location_runtime_t> ();
    location_runtime.start (*registration->routing_id);
    runtime::mesh_node_host_service_t service ({registration}, serializers);
    service.start (provider);
    const auto node = service.nodes ().front ();
    auto native_spot = std::make_shared<host::spot_handle_t> (
      node->native_node ().get_or_create_spot (std::string (context->spot_id)));
    registration->spot_state->lane.run ([&] { context->native_spot = native_spot; }).get ();

    messaging::client_call_codec_t route_codec;
    auto multicast_header = route_codec.create_envelope (messaging::message_kind_t::command, "work",
                                                         spot_multicast_route_send_t::packet_name);
    const auto multicast = spot_multicast_route_send_t{
      "rel-358-topic", make_route_multicast_frame ("Rel358MulticastPayload", "rel-358-topic",
                                                   "multicast-through-host")
                         .to_bytes ()};
    auto multicast_parts =
      route_codec.encode_envelope_parts (multicast_header, multicast, serializers);
    host::ready_record_t channel_owner;
    channel_owner.owner_kind = host::owner_kind_t::channel;
    channel_owner.channel_name = "work";
    host::receive_record_t channel_record;
    channel_record.kind = host::record_kind_t::channel_send;
    channel_record.channel_name = "work";
    channel_record.source_node_rid = zlink::routing_id_t::from ("route-source");
    enqueue_application_route (node, std::move (channel_owner), std::move (channel_record),
                               std::move (multicast_parts).take_items ());
    assert (multicast_future.wait_for (1s) == std::future_status::ready);
    assert (multicast_future.get () == "multicast-through-host");

    auto disconnect_header =
      route_codec.create_envelope (messaging::message_kind_t::request, "work",
                                   spot_actor_disconnect_route_request_t::packet_name);
    auto disconnect_parts = route_codec.encode_envelope_parts (
      disconnect_header, make_spot_actor_disconnect_route_request (actor), serializers);
    auto disconnect_reply = std::make_shared<std::promise<std::vector<zlink::message_t>>> ();
    auto disconnect_reply_future = disconnect_reply->get_future ();
    host::ready_record_t node_owner;
    node_owner.owner_kind = host::owner_kind_t::node;
    host::receive_record_t node_record;
    node_record.kind = host::record_kind_t::node_request;
    node_record.source_node_rid = zlink::routing_id_t::from ("route-source");
    node_record.reply_token.host = node->native_node ().shared_from_this ();
    node_record.reply_token.local_reply =
      [disconnect_reply] (const std::vector<zlink::message_t> &parts) {
          disconnect_reply->set_value (parts);
          return true;
      };
    enqueue_application_route (node, std::move (node_owner), std::move (node_record),
                               std::move (disconnect_parts).take_items ());
    assert (disconnect_reply_future.wait_for (1s) == std::future_status::ready);
    auto reply_parts = messaging::message_parts_t (disconnect_reply_future.get ());
    const auto reply = route_codec.decode_envelope_reply<spot_actor_disconnect_route_reply_t> (
      reply_parts, serializers, "disconnect reply is empty", "disconnect reply is invalid",
      "actor disconnect");
    assert (reply && reply.value ().accepted);
    assert (disconnect_called.load (std::memory_order_acquire));
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));

    service.stop ();
    location_runtime.stop ();
}

void verify_local_join_timeout_releases_membership ()
{
    namespace host = zlink::framework::runtime::host;
    using namespace std::chrono_literals;
    auto state = make_node ("tcp://127.0.0.1:*", "join-timeout-owner");
    zlink::framework::detail::mesh_node_runtime_t node (state);
    node.start ();
    auto &runtime = node.native_node ();
    auto actor = runtime.create_actor ("vertical.actor", "join-timeout-actor");
    auto target = runtime.get_or_create_spot ("join-timeout-spot");
    const auto rid = runtime.status ().routing_id ();
    const auto generation = target.status ().lifecycle_generation ();
    host::pending_operation_t expired;
    assert (actor.join_spot (rid, "join-timeout-spot", generation, {}, expired, 25ms)
            == zlink::submit_result_t::ok);
    assert (runtime.transport ().expire_requests (std::chrono::steady_clock::now () + 1s) == 1);
    auto first = expired.completion->task ();
    assert (!first.result ());
    assert (first.result ().error_kind ()
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (zlink::framework::detail::boundary_state (*first.result ().error ())
            == zlink::framework::detail::boundary_error_t::timed_out);

    // The expired queued control is skipped, and the same Actor can start
    // another membership move because the terminal owner released its token.
    auto &retried = expired;
    assert (actor.join_spot (rid, "join-timeout-spot", generation, {}, retried, 1s)
            == zlink::submit_result_t::ok);
    bool replied = false;
    (void) runtime
      .dispatch_ready ([&] (const host::ready_record_t &, const host::receive_record_t &record,
                            std::vector<zlink::message_t>) {
          if (record.kind == host::record_kind_t::spot_control)
              replied = host::actor_join_reply (record.reply_token,
                                                host::actor_join_result_t::accepted, {});
      })
      .result ()
      .value ();
    assert (replied);
    auto second = retried.completion->task ();
    assert (second.result ());
    assert (second.result ().value ().record.join_completion);
    assert (second.result ().value ().record.join_completion->join_result
            == host::join_admission_t::accepted);
    assert (!first.result ());
    node.stop ();
}

void register_mesh_location_resolvers (zlink::framework::service_collection_t &services)
{
    services.add_factory<zlink::framework::runtime::store_location_resolvers_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::runtime::store_location_resolvers_t> (
            provider.get_required<zlink::framework::location_repository_t> ());
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::spot_address_resolver_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::shared_ptr<zlink::framework::runtime::spot_address_resolver_t> (
            &provider.get_required<zlink::framework::runtime::store_location_resolvers_t> (),
            [] (zlink::framework::runtime::spot_address_resolver_t *) noexcept {});
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::actor_address_resolver_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::shared_ptr<zlink::framework::runtime::actor_address_resolver_t> (
            &provider.get_required<zlink::framework::runtime::store_location_resolvers_t> (),
            [] (zlink::framework::runtime::actor_address_resolver_t *) noexcept {});
      },
      zlink::framework::service_lifetime_t::singleton);
}

class expired_actor_bind_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    expired_actor_bind_session_t (zlink::framework::session_actor_manager_t &manager,
                                  zlink::framework::actor_ref_t actor,
                                  std::promise<zlink::framework::framework_error_kind_t> &result) :
        _manager (manager), _actor (std::move (actor)), _result (result)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        try {
            (void) co_await _manager.bind (_actor).async ();
            _result.set_value (zlink::framework::framework_error_kind_t::internal_failure);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            _result.set_value (error.kind ());
        }
        co_return;
    }
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::message_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::session_actor_manager_t &_manager;
    zlink::framework::actor_ref_t _actor;
    std::promise<zlink::framework::framework_error_kind_t> &_result;
};

void verify_actor_route_resolver_preserves_unavailable ()
{
    using namespace zlink::framework;
    auto registration = make_named_node ("actor-route-unavailable", "actor-route-unavailable-node");
    const auto actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("expired-actor-target"), "player", "expired-actor", 1);
    std::promise<framework_error_kind_t> bind_result;
    auto bind_terminal = bind_result.get_future ();
    serializer_registry_t serializers;
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);
    services.add_factory<expired_actor_bind_session_t> (
      [actor, &bind_result] (service_provider_t &provider) {
          return std::make_unique<expired_actor_bind_session_t> (
            provider.get_required<session_actor_manager_t> (), actor, bind_result);
      },
      service_lifetime_t::scoped);
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<runtime::location_runtime_t> ().start (*registration->routing_id);
    runtime::mesh_node_host_service_t service ({registration}, serializers);
    service.start (provider);
    const auto claimed = store.claim_owner_lease ("expired-actor-owner", 200ms).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claimed);
    assert (owner);
    const auto target_rid = zlink::routing_id_t::from ("expired-actor-target");
    mesh_node_descriptor_t descriptor{
      .mesh_name = "actor-route-unavailable",
      .rid = target_rid,
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:5001",
      .application_version = 1,
      .object_capabilities = {{.object_kind = placement_object_kind_t::actor,
                               .stable_type = "player",
                               .policy = maintenance_policy_kind_t::recreate}},
      .object_role = object_role_t::server,
      .capacity = {.actors = {.limit = 1}},
      .activation_concurrency = {.limit = 1},
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner->token.owner_id,
      .lease_generation = owner->token.lease_generation};
    assert (store.update_mesh_node (descriptor, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);
    const object_reserve_request_t reservation{
      .key = {placement_object_kind_t::actor, "expired-actor"},
      .intent = {.stable_type = "player",
                 .request_content_reference = "test",
                 .request_encoded_size = 1},
      .target = {.mesh_name = "actor-route-unavailable",
                 .node_rid = node_rid_t::from_string ("expired-actor-target"),
                 .node_lifecycle_generation = 1,
                 .owner = owner->token},
      .creating_payload = {std::byte{1}},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = store.reserve (reservation).result ().value ();
    const auto *pending = std::get_if<object_reserved_t> (&reserved);
    assert (pending);
    const auto payload = runtime::encode_actor_authority_payload (actor, "spot-a", 1);
    const auto committed =
      store.commit ({reservation.key, pending->fence, payload}).result ().value ();
    assert (std::get_if<object_committed_t> (&committed));
    std::this_thread::sleep_for (220ms);
    const auto retained =
      store.read_authority (runtime::actor_authority_key ("expired-actor")).result ().value ();
    assert (std::get_if<authority_snapshot_t> (&retained));
    const auto expired_lease = store.read_owner_lease (owner->token.owner_id).result ().value ();
    assert (std::get_if<owner_lease_missing_t> (&expired_lease));
    handler_registry_t handlers;
    zlink_builder_t stream_zlink;
    detail::configure_stream_dispatch_executor ();
    zlink_framework_options_t stream_options (services, handlers, serializers, stream_zlink);
    auto stream_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    stream_options.add_stream_node ("expired-actor-stream")
      .bind ("tcp://127.0.0.1:0")
      .register_session ("expired-actor-session");
    stream_options.apply ();
    auto stream_runtime = detail::stream_runtime_t::from (stream_zlink);
    const auto stream_snapshots = stream_runtime.snapshots ();
    assert (stream_snapshots.size () == 1);
    assert (stream_snapshots.front ().packet_session_name == "expired-actor-session");
    runtime::stream_host_service_t stream_host (
      stream_runtime, stream_snapshots,
      {{"expired-actor-session",
        [] (service_provider_t &scope) -> packet_stream_session_t & {
            return scope.get_required<expired_actor_bind_session_t> ();
        }}},
      30s, service.nodes ().front (), {}, stream_listeners);
    assert (stream_host.start (provider).result ());
    const auto stream_endpoint =
      stream_listeners->find (zlink::framework::listener_kind_t::stream, "expired-actor-stream")
        ->endpoint;
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = stream_endpoint;
    connector_options.connect_timeout = 2s;
    connector_options.reconnect.enabled = false;
    auto client = zlink::stream_connector::connector_factory_t::create (connector_options);
    assert (client.connect ());
    assert (bind_terminal.wait_for (2s) == std::future_status::ready);
    const auto terminal = bind_terminal.get ();
    if (terminal != framework_error_kind_t::unavailable)
        std::cerr << "expired authority STREAM bind kind=" << static_cast<int> (terminal)
                  << std::endl;
    assert (terminal == framework_error_kind_t::unavailable);
    (void) client.close ();
    stream_host.stop ();
    service.stop ();
}

zlink::framework::framework_runtime_state_t
read_mesh_state (faulting_mesh_location_repository_t &store,
                 const std::string &mesh_name,
                 const zlink::routing_id_t &rid)
{
    const auto page = store.list_mesh_nodes (mesh_name).result ().value ();
    const auto found =
      std::find_if (page.items.begin (), page.items.end (),
                    [&rid] (const zlink::framework::mesh_node_descriptor_t &descriptor) {
                        return descriptor.rid.to_hex () == rid.to_hex ();
                    });
    assert (found != page.items.end ());
    return found->state;
}

void verify_server_descriptor_publishes_framework_entry_spot_without_application_entry ()
{
    auto registration = make_named_node ("descriptor-entry", "descriptor-entry-node");
    registration->object_role = zlink::framework::object_role_t::server;
    assert (!registration->spot_state->snapshot.entry_spot_name);

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);

    zlink::framework::runtime::mesh_node_host_service_t service ({registration}, serializers);
    service.start (provider);
    const auto nodes = store.list_mesh_nodes ("descriptor-entry").result ().value ();
    assert (nodes.items.size () == 1);
    const auto &descriptor = nodes.items.front ();
    assert (descriptor.entry_spot_id);
    assert (!descriptor.entry_spot_id->empty ());
    assert (*descriptor.entry_spot_id
            == service.nodes ().front ()->native_node ().entry_spot ().spot_id ());
    service.stop ();
}

void verify_descriptor_retire_order_and_pre_seal_rollback ()
{
    using zlink::framework::framework_runtime_state_t;

    auto first = make_named_node ("vertical-order", "order-a");
    auto second = make_named_node ("vertical-order", "order-b");
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store = std::make_unique<faulting_mesh_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *first->routing_id);
    zlink::framework::runtime::mesh_node_host_service_t service ({first, second}, serializers);
    service.start (provider);

    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::serving);
    assert (read_mesh_state (store, "vertical-order", *second->routing_id)
            == framework_runtime_state_t::serving);

    // A partial Retiring publication is a pre-seal failure. The caller can
    // republish Serving before it closes admission, leaving both descriptors
    // selectable and the local runtime unchanged.
    store.fail_next_retiring_write (2);
    assert (!service.publish_descriptor_state (framework_runtime_state_t::relocating));
    assert (service.publish_descriptor_state (framework_runtime_state_t::serving));
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::serving);
    assert (read_mesh_state (store, "vertical-order", *second->routing_id)
            == framework_runtime_state_t::serving);

    // Once preflight succeeds, the externally visible order is Retiring
    // before the dispatch seal and Draining only after relocation succeeds.
    assert (service.publish_descriptor_state (framework_runtime_state_t::relocating));
    service.seal_application_dispatch ();
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::relocating);
    assert (service.publish_descriptor_state (framework_runtime_state_t::draining));
    assert (read_mesh_state (store, "vertical-order", *first->routing_id)
            == framework_runtime_state_t::draining);

    for (const auto &node : service.nodes ()) {
        assert (node->status ().state
                == zlink::framework::runtime::host::node_status_t::state_t::draining);
    }

    const auto history = store.state_history ();
    const auto first_retiring =
      std::find (history.begin (), history.end (), framework_runtime_state_t::relocating);
    const auto first_draining =
      std::find (first_retiring, history.end (), framework_runtime_state_t::draining);
    assert (first_retiring != history.end ());
    assert (first_draining != history.end ());
    assert (first_retiring < first_draining);
    service.stop ();
}

struct local_route_probe_message_t
{
    std::string value;
};

struct local_route_probe_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool gate_open = false;
    int entered = 0;
    int completed = 0;
    std::vector<std::string> values;
    std::string last_mesh_name;
    std::string last_channel_name;
    std::string last_packet_name;
    std::string last_content_type;
    std::string last_source_node_rid;
};

class local_route_probe_handler_t
{
  public:
    explicit local_route_probe_handler_t (std::shared_ptr<local_route_probe_state_t> state) :
        _state (std::move (state))
    {
    }

    void handle (const local_route_probe_message_t &message,
                 const zlink::framework::route_message_context_t &context)
    {
        std::unique_lock lock (_state->mutex);
        ++_state->entered;
        // v11 MessageContext: node-direct dispatch has no ChannelName, so the router channel
        // identity travels in the Mesh name and only the source node RID is added.
        _state->last_mesh_name = context.mesh_name.value_or ("<none>");
        _state->last_channel_name = context.channel_name.value_or ("<none>");
        _state->last_packet_name = context.packet_name;
        _state->last_content_type = context.content_type.value_or ("<none>");
        _state->last_source_node_rid = context.source_node_rid.to_string ();
        _state->values.push_back (message.value);
        _state->changed.notify_all ();
        if (message.value == "throw")
            throw std::runtime_error ("local route probe failure");
        _state->changed.wait (lock, [this] { return _state->gate_open; });
        ++_state->completed;
        _state->changed.notify_all ();
    }

  private:
    std::shared_ptr<local_route_probe_state_t> _state;
};

void verify_local_node_submit_bridge ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "local-route-node");
    registration->handlers.on_send<local_route_probe_handler_t, local_route_probe_message_t> (
      "vertical-mesh", "LocalRouteProbe", &local_route_probe_handler_t::handle);

    auto independent_registration = make_named_node ("independent-mesh", "independent-route-node");
    independent_registration->handlers
      .on_send<local_route_probe_handler_t, local_route_probe_message_t> (
        "independent-mesh", "LocalRouteProbe", &local_route_probe_handler_t::handle);

    zlink::framework::serializer_registry_t serializers;
    serializers.add<local_route_probe_message_t> (
      [] (const local_route_probe_message_t &message) {
          return zlink::framework::encoded_payload_t::from_string (message.value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return local_route_probe_message_t{payload.to_string ()};
      });
    auto probe = std::make_shared<local_route_probe_state_t> ();
    zlink::framework::service_collection_t services;
    services.add_singleton<local_route_probe_handler_t> (
      std::make_unique<local_route_probe_handler_t> (probe));
    // v11: the MeshNode host resolves the Location store from the provider, so
    // a vertical check registers the in-memory store like any application.
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    // The MeshNode publishes its descriptor under an owner lease, so the
    // Location runtime starts first just as the host does in production.
    auto &location_runtime =
      provider.get_required<zlink::framework::runtime::location_runtime_t> ();
    location_runtime.start (*registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service (
      {registration, independent_registration}, serializers, {}, {}, application_jobs);
    service.start (provider);
    const auto node = service.nodes ().front ();
    const auto independent_node = service.nodes ().back ();

    auto encode = [&serializers] (std::string value, std::string mesh_name = "vertical-mesh") {
        zlink::framework::runtime::messaging::client_call_codec_t codec;
        const auto header =
          codec.create_envelope (zlink::framework::runtime::messaging::message_kind_t::command,
                                 std::move (mesh_name), "LocalRouteProbe");
        return codec.encode_envelope_parts (header, local_route_probe_message_t{std::move (value)},
                                            serializers);
    };

    {
        auto parts = encode ("owned-after-return");
        assert (service.submit_local_node_send (node, parts.items ())
                == zlink::submit_result_t::ok);
    }
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 1; }));
        assert (probe->completed == 0);
    }
    {
        // The receive pump shares this queue and can reserve the released
        // permit. Acquire it through the same FIFO to prove that the running
        // handler returned capacity at entry, independent of pump scheduling.
        using permit_t = zlink::framework::runtime::application_job_queue_t::permit_t;
        auto released_capacity = std::make_shared<std::promise<std::optional<permit_t>>> ();
        auto available = released_capacity->get_future ();
        auto waiter =
          application_jobs->wait_for_supply ([released_capacity] (std::optional<permit_t> permit) {
              released_capacity->set_value (std::move (permit));
          });
        assert (available.wait_for (1s) == std::future_status::ready);
        auto permit = available.get ();
        assert (permit);
        const auto capacity_while_handlers_are_pending = application_jobs->snapshot ();
        assert (capacity_while_handlers_are_pending.effective_max_queued_application_jobs == 1);
        assert (capacity_while_handlers_are_pending.reserved_supply_permits == 1);
        assert (capacity_while_handlers_are_pending.queued_application_jobs == 0);
        assert (capacity_while_handlers_are_pending.permits_in_use == 1);
    }

    // A different node owns a different gate and must enter while the first
    // handler is blocked. The same-node successor retains its permit until
    // that node's first handler reaches terminal completion (02 §2).
    auto independent = encode ("independent-owner", "independent-mesh");
    assert (service.submit_local_node_send (independent_node, independent.items ())
            == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 2; }));
        assert (probe->completed == 0);
    }
    auto progresses = encode ("progress-after-entry");
    assert (service.submit_local_node_send (node, progresses.items ())
            == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (!probe->changed.wait_for (lock, 1s, [&] { return probe->entered == 3; }));
        assert (probe->completed == 0);
        assert (node->pending_application_callbacks () == 1);
        assert (node->active_application_callbacks () == 1);
        assert (independent_node->active_application_callbacks () == 1);
        const auto queued = application_jobs->snapshot ();
        assert (queued.effective_max_queued_application_jobs == 1);
        assert (queued.queued_application_jobs == 1);
        assert (queued.permits_in_use == 1);
    }

    {
        std::lock_guard lock (probe->mutex);
        probe->gate_open = true;
    }
    probe->changed.notify_all ();
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->completed == 3; }));
        assert ((probe->values
                 == std::vector<std::string>{"owned-after-return", "independent-owner",
                                             "progress-after-entry"}));
        assert (probe->last_mesh_name == "vertical-mesh");
        assert (probe->last_channel_name == "<none>");
        assert (probe->last_packet_name == "LocalRouteProbe");
        // The wire content type, not the struct default, so the value really travels.
        assert (probe->last_content_type == "application/octet-stream");
        assert (!probe->last_source_node_rid.empty ());
    }
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));

    auto throwing = encode ("throw");
    assert (service.submit_local_node_send (node, throwing.items ()) == zlink::submit_result_t::ok);
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    auto after_throw = encode ("after-throw");
    assert (service.submit_local_node_send (node, after_throw.items ())
            == zlink::submit_result_t::ok);
    {
        std::unique_lock lock (probe->mutex);
        assert (probe->changed.wait_for (lock, 1s, [&] { return probe->completed == 4; }));
    }
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);

    service.seal_application_dispatch ();
    auto after_seal = encode ("after-seal");
    assert (service.submit_local_node_send (node, after_seal.items ())
            == zlink::submit_result_t::terminated);
    service.stop ();
    location_runtime.stop ();
    assert (!location_runtime.current_owner_token ());
}

// Regression pin for mesh_node_runtime_t::classify_node_direct_target's
// Location-Store gate. A route-only MeshNode's direct-target send/request
// must consult the real transport-level admission state
// (raw_mesh_node_owner_t::request_with_header's `_topology.peer(...)` gate),
// not fail with a blanket not_found merely because the target routing id is
// absent from this node's own Location Store page. Route-only
// (object_role=none) MeshNode peers are published for route/channel topology,
// but their descriptors expose no object placement capability. The absent
// entry below is therefore the unknown target, not this source node.
// classify_node_direct_target used to misclassify every direct-target
// send/request as not_found before the network call was attempted, regardless
// of whether the target was actually admitted. This node never admits any
// peer at all, so the correct (post-fix) outcome for an unknown target is
// not_connected -- the real topology-backed answer.
// Before the fix, classify_node_direct_target returned not_found here
// unconditionally without ever consulting the transport layer, so this
// assertion would have failed pre-fix and pins the fix now.
void verify_direct_target_falls_through_absent_location_store_entry ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "location-gate-node");
    registration->object_role = zlink::framework::object_role_t::none;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    // v11: the MeshNode host resolves the Location store from the provider,
    // so this check registers a real (but empty) in-memory store like any
    // application would. The store never gets a "target" entry for the
    // routing id used below.
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service ({registration}, serializers, {},
                                                                 {}, application_jobs);
    service.start (provider);
    const auto node = service.nodes ().front ();

    const auto descriptors = location_store.list_mesh_nodes ("vertical-mesh").result ().value ();
    const auto source_descriptor =
      std::find_if (descriptors.items.begin (), descriptors.items.end (), [&] (const auto &item) {
          return item.rid.to_hex () == registration->routing_id->to_hex ();
      });
    assert (source_descriptor != descriptors.items.end ());
    assert (source_descriptor->object_role == zlink::framework::object_role_t::none);
    assert (source_descriptor->object_capabilities.empty ());
    assert (!source_descriptor->entry_spot_id);
    assert (source_descriptor->channel_weights.contains ("work"));

    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("direct"))};
    const auto target = zlink::routing_id_t::from (std::string ("never-admitted-target"));
    const auto result = std::move (node->send_to_node (target, parts, std::vector<std::uint8_t>{}))
                          .result ()
                          .value ();
    assert (result == zlink::submit_result_t::not_connected);

    service.stop ();
}

// Residual-convergence pin (spec 32-framework-error-model.ko.md:76-77): a
// `Request` to a target this runtime has NEVER admitted (absent from
// service_topology_registry_t's live peer table, distinct from the `Send`
// case above whose target routing id merely never appears in the Location
// Store page) must complete NotFound, not Unavailable/not_connected.
// public_host_runtime_t::request_to_node used to fall back to
// `submitted(false)` unconditionally, which collapses every rejected
// request into `not_connected` regardless of whether the target was ever
// seen; that violates the spec's split between "target does not exist"
// (NotFound, line 76) and "target exists but is unreachable" (Unavailable,
// line 77). Matches Java's ZLinkJavaRawSpotNode.classifyNodeSendTarget
// (peerState absent -> TARGET_NOT_FOUND) and Node's
// raw-service-mesh-runtime.ts knownTarget gate.
void verify_request_to_never_admitted_target_reports_not_found ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "never-admitted-request-node");
    registration->object_role = zlink::framework::object_role_t::none;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_collection_t services;
    auto owned_store =
      std::make_unique<zlink::framework::runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<zlink::framework::location_repository_t> (
      std::unique_ptr<zlink::framework::location_repository_t> (owned_store.release ()));
    services.add_singleton<zlink::framework::runtime::location_runtime_t> (
      std::make_unique<zlink::framework::runtime::location_runtime_t> (location_store));
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    provider.get_required<zlink::framework::runtime::location_runtime_t> ().start (
      *registration->routing_id);
    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    zlink::framework::runtime::mesh_node_host_service_t service ({registration}, serializers, {},
                                                                 {}, application_jobs);
    service.start (provider);
    const auto node = service.nodes ().front ();

    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("request"))};
    const auto target = zlink::routing_id_t::from (std::string ("never-admitted-request-target"));
    zlink::framework::runtime::host::pending_operation_t operation_id;
    const auto result =
      std::move (node->request_to_node (target, parts, operation_id, std::chrono::milliseconds (25),
                                        std::vector<std::uint8_t>{}))
        .result ()
        .value ();
    assert (result == zlink::submit_result_t::not_found);

    service.stop ();
}

void verify_public_runtime_surface ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "runtime-a");
    registration->channels.emplace (
      "client-only", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, false});
    registration->actor_limit = 0;
    registration->spot_limit = 17;
    registration->spot_state->activation_admission->set_limit (5);
    registration->placement_weight = 42;
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ();
    monitoring_mesh_store_t monitoring_store;
    const auto node_status = node->status ();
    zlink::framework::mesh_node_descriptor_t local_descriptor;
    local_descriptor.mesh_name = "vertical-mesh";
    local_descriptor.rid = node_status.routing_id ();
    local_descriptor.lifecycle_generation = node_status.lifecycle_generation ();
    local_descriptor.descriptor_revision = 3;
    local_descriptor.object_role = zlink::framework::object_role_t::server;
    local_descriptor.channel_weights.emplace ("work", 100);
    local_descriptor.state = zlink::framework::framework_runtime_state_t::serving;
    local_descriptor.placement_weight = 42;
    local_descriptor.capacity = {
      .actors = {.active = 4, .reserved = 2, .limit = 0},
      .spots = {.active = 3, .reserved = 1, .limit = 17},
      .spot_types = {{.object_kind = zlink::framework::placement_object_kind_t::user_spot,
                      .stable_type = "room",
                      .usage = {.active = 3, .reserved = 1, .limit = 9}}}};
    local_descriptor.activation_concurrency = {.active = 2, .limit = 5};
    monitoring_store.set_local (local_descriptor);
    assert (registration->listen_endpoint == node->status ().local_endpoint ());
    assert (registration->listen_endpoint != "tcp://127.0.0.1:0");
    auto runtime = std::make_shared<zlink::framework::runtime::route_mesh_runtime_service_t> (
      std::vector<std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t>>{node}, nullptr,
      &monitoring_store);
    runtime->start ();
    zlink::framework::runtime::route_mesh_runtime_options_service_t runtime_options ({node});

    const auto first = runtime->snapshot ("vertical-mesh");
    const auto second = runtime->snapshot ("vertical-mesh");
    assert (first.mesh_name == "vertical-mesh");
    assert (first.state == zlink::framework::mesh_node_state_t::ready);
    assert (first.is_ready);
    // Status counts come from this MeshNode's activation records, not from the
    // Location Store projection (runtime monitoring §5): nothing is active here.
    assert (first.placement.active_actor_count == 0);
    assert (first.placement.active_spot_count == 0);
    assert (first.placement.is_available);
    assert (first.channels.size () == 2);
    const auto client_only_initial =
      std::find_if (first.channels.begin (), first.channels.end (),
                    [] (const auto &channel) { return channel.channel_name == "client-only"; });
    assert (client_only_initial != first.channels.end ());
    assert (!client_only_initial->is_ready);
    assert (client_only_initial->ready_target_count == 0);
    const auto work_initial =
      std::find_if (first.channels.begin (), first.channels.end (),
                    [] (const auto &channel) { return channel.channel_name == "work"; });
    assert (work_initial != first.channels.end ());
    assert (work_initial->ready_target_count == 1);
    assert (work_initial->is_ready);
    assert (second.sequence > first.sequence);
    auto &channel_options = runtime_options.channel ("work");
    channel_options.weight (0);
    assert (channel_options.weight () == 0);
    assert (!runtime->snapshot ("vertical-mesh").channels.front ().is_ready);
    channel_options.weight (100);
    assert (channel_options.weight () == 100);
    runtime_options.mesh ("vertical-mesh").placement_weight (0);
    assert (runtime_options.mesh ("vertical-mesh").placement_weight () == 0);
    runtime_options.mesh ("vertical-mesh").placement_weight (100);
    assert (runtime_options.mesh ("vertical-mesh").placement_weight () == 100);

    std::mutex event_mutex;
    std::condition_variable event_ready;
    std::vector<zlink::framework::mesh_node_snapshot_t> received;
    std::this_thread::sleep_for (150ms);
    auto observation = runtime->observe (
      "vertical-mesh", 1,
      [&] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t>
             &observed) {
          const auto &status = observed.status;
          {
              std::lock_guard lock (event_mutex);
              received.push_back (status);
          }
          event_ready.notify_one ();
      });
    auto wait_for_snapshot = [&] (const auto &predicate) {
        std::unique_lock lock (event_mutex);
        return event_ready.wait_for (
          lock, 2s, [&] { return !received.empty () && predicate (received.back ()); });
    };
    {
        std::unique_lock lock (event_mutex);
        assert (event_ready.wait_for (lock, 2s, [&] { return !received.empty (); }));
        assert (received.back ().mesh_name == "vertical-mesh");
        assert (received.back ().state == zlink::framework::mesh_node_state_t::ready);
    }

    auto remote_location = local_descriptor;
    remote_location.rid = zlink::routing_id_t::from (std::string ("runtime-peer"));
    remote_location.lifecycle_generation = 99;
    remote_location.descriptor_revision = 1;
    remote_location.endpoint = "tcp://127.0.0.1:39999";
    remote_location.channel_weights.emplace ("client-only", 100);
    monitoring_store.set_remote (remote_location);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::degraded && !snapshot.is_ready
               && std::any_of (
                 snapshot.peers.begin (), snapshot.peers.end (), [] (const auto &peer) {
                     return peer.state == zlink::framework::peer_state_t::not_connected;
                 });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));

    auto remote_service = node->native_node ().transport ().topology ().local_descriptor ();
    remote_service.node_routing_id = remote_location.rid.to_bytes ();
    remote_service.lifecycle_generation = remote_location.lifecycle_generation;
    remote_service.descriptor_revision = remote_location.descriptor_revision;
    remote_service.advertised_endpoint = remote_location.endpoint;
    remote_service.channels.push_back (
      zlink::framework::runtime::mesh::service_channel_descriptor_t{"client-only", 100});
    std::sort (remote_service.channels.begin (), remote_service.channels.end (),
               [] (const auto &left, const auto &right) { return left.name < right.name; });
    const std::vector<std::uint8_t> connection_id{0x41, 0x42};
    assert (node->native_node ().transport ().topology ().admit (remote_service, connection_id)
            == zlink::framework::runtime::mesh::peer_admission_result_t::admitted);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::ready
               && snapshot.ready_peer_count == 1;
    }));
    assert (runtime->is_ready ("vertical-mesh"));
    {
        const auto current = runtime->snapshot ("vertical-mesh");
        const auto client_only =
          std::find_if (current.channels.begin (), current.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "client-only"; });
        assert (client_only != current.channels.end ());
        assert (client_only->is_ready);
        assert (client_only->ready_target_count == 1);
    }

    channel_options.weight (0);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        const auto work =
          std::find_if (snapshot.channels.begin (), snapshot.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "work"; });
        return work != snapshot.channels.end () && work->ready_target_count == 1;
    }));
    channel_options.weight (100);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        const auto work =
          std::find_if (snapshot.channels.begin (), snapshot.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "work"; });
        return work != snapshot.channels.end () && work->ready_target_count == 2;
    }));

    assert (node->native_node ().transport ().topology ().disconnect (
      remote_service.node_routing_id, connection_id));
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::degraded;
    }));
    assert (!runtime->is_ready ("vertical-mesh"));
    monitoring_store.set_remote (std::nullopt);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::ready
               && snapshot.peers.empty ();
    }));
    assert (runtime->is_ready ("vertical-mesh"));

    auto unavailable = monitoring_store.local ();
    ++unavailable.descriptor_revision;
    unavailable.placement_weight = 0;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return !snapshot.placement.is_available
               && snapshot.placement.unavailable_reason
                    == zlink::framework::topology_reason_t::capacity_exceeded;
    }));

    unavailable.placement_weight = 100;
    // The reserved slots alone exhaust both limits (MeshNode §5.1).
    unavailable.capacity.actors.limit = 2;
    unavailable.capacity.spots.limit = 1;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (
      wait_for_snapshot ([] (const auto &snapshot) { return !snapshot.placement.is_available; }));

    unavailable.capacity.spots.limit = 17;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (
      wait_for_snapshot ([] (const auto &snapshot) { return snapshot.placement.is_available; }));

    unavailable.state = zlink::framework::framework_runtime_state_t::relocating;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::stopping && !snapshot.is_ready
               && std::all_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) { return !channel.is_ready; })
               && std::any_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) { return channel.ready_target_count > 0; });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));

    unavailable.state = zlink::framework::framework_runtime_state_t::serving;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::ready && snapshot.is_ready;
    }));

    bool rejected_capacity = false;
    try {
        (void) runtime->observe (
          "vertical-mesh", 0,
          [] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t> &) {
          });
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_capacity = true;
    }
    assert (rejected_capacity);

    bool rejected_mesh = false;
    try {
        (void) runtime->snapshot ("missing");
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_mesh = true;
    }
    assert (rejected_mesh);
    assert (runtime->is_ready ("vertical-mesh"));

    runtime->stop ();
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::mesh_node_state_t::stopped && !snapshot.is_ready
               && snapshot.ready_peer_count == 0
               && std::all_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) {
                                   return !channel.is_ready && channel.ready_target_count == 0;
                               });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));
    assert (runtime->snapshot ("vertical-mesh").state
            == zlink::framework::mesh_node_state_t::stopped);
    observation->close ();
    node->stop ();
}

void verify_automatic_identity_and_port_builder ()
{
    zlink::framework::zlink_builder_t builder;
    auto mesh = builder.add_route_mesh ("automatic-builder-mesh");
    mesh.set_bind_host ("127.0.0.1")
      .listen (std::uint16_t{0})
      .set_automatic_routing_id_prefix ("play.node");

    const auto registrations =
      zlink::framework::detail::mesh_node_runtime_t::registrations (builder);
    assert (registrations.size () == 1);
    const auto &state = registrations.front ();
    assert (state->listen_endpoint == "tcp://127.0.0.1:0");
    assert (state->routing_id);
    assert (std::regex_match (
      state->routing_id->to_string (),
      std::regex (
        R"(play\.node-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})")));
    assert (state->spot_state->snapshot.routing_id
            && state->spot_state->snapshot.routing_id->to_string ()
                 == state->routing_id->to_string ());

    bool rejected_fixed_id = false;
    try {
        mesh.set_routing_id (zlink::routing_id_t::from ("fixed"));
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_fixed_id = true;
    }
    assert (rejected_fixed_id);

    zlink::framework::zlink_builder_t invalid_builder;
    auto invalid_mesh = invalid_builder.add_route_mesh ("invalid-builder-mesh");
    bool rejected_prefix = false;
    try {
        invalid_mesh.set_automatic_routing_id_prefix ("bad prefix");
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_prefix = true;
    }
    assert (rejected_prefix);
}

void verify_slow_observer_does_not_block_stop ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "slow-observer");
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ();
    auto runtime = std::make_shared<zlink::framework::runtime::route_mesh_runtime_service_t> (
      std::vector<std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t>>{node}, nullptr);
    runtime->start ();
    zlink::framework::runtime::route_mesh_runtime_options_service_t runtime_options ({node});

    std::mutex gate;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::size_t stopped_count = 0;
    auto observation = runtime->observe (
      "vertical-mesh", 4,
      [&] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t>
             &observed) {
          const auto &snapshot = observed.status;
          std::unique_lock lock (gate);
          if (!entered) {
              entered = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return released; });
          }
          if (snapshot.state == zlink::framework::mesh_node_state_t::stopped) {
              ++stopped_count;
              changed.notify_all ();
          }
      });
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return entered; }));
    }

    // Queue a non-terminal change while the callback is blocked. Stop must
    // replace it with one immutable terminal snapshot before the runtime is
    // destroyed; the callback must never read the destroyed runtime.
    runtime_options.channel ("work").weight (0);
    auto stopping = std::async (std::launch::async, [&] { runtime->stop (); });
    assert (stopping.wait_for (1s) == std::future_status::ready);
    stopping.get ();
    runtime.reset ();
    {
        std::lock_guard lock (gate);
        released = true;
    }
    changed.notify_all ();
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return stopped_count == 1; }));
    }
    std::this_thread::sleep_for (50ms);
    {
        std::lock_guard lock (gate);
        assert (stopped_count == 1);
    }
    observation->close ();
    node->stop ();
}

void verify_host_shutdown_seal_reaches_raw_mesh ()
{
    namespace mesh = zlink::framework::runtime::mesh;
    auto registration = make_node ("tcp://127.0.0.1:0", "seal-owner");
    auto seal = std::make_shared<std::atomic_bool> (false);
    zlink::framework::detail::spot_node_runtime_t (registration->spot_state).bind_drain_flag (seal);
    zlink::framework::detail::mesh_node_runtime_t node (registration);
    node.start ();
    auto remote_options = mesh::raw_mesh_node_options_t{
      node.native_node ().transport ().topology ().local_descriptor ()};
    remote_options.descriptor.node_routing_id = {'s', 'i', 'l', 'e', 'n', 't'};
    remote_options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    remote_options.descriptor.state = mesh::service_node_state_t::preparing;
    remote_options.shutdown_admission_seal = seal;
    mesh::raw_mesh_node_owner_t remote (std::move (remote_options));
    remote.start ();
    seal->store (true, std::memory_order_release);
    auto &transport = node.native_node ().transport ();
    assert (transport.connect_peer (remote.endpoint (), remote.topology ().local_descriptor ()));
    std::size_t ready = 0;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (ready == 0 && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        transport.drain_monitor_events (now).result ().value ();
        ready += remote.drain_monitor_events (now).result ().value ();
        std::this_thread::sleep_for (1ms);
    }
    assert (ready != 0);
    const auto quiet_until = std::chrono::steady_clock::now () + 200ms;
    while (std::chrono::steady_clock::now () < quiet_until) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        transport.drain_monitor_events (now).result ().value ();
        assert (remote.pump_one (now).result ().value () == mesh::raw_mesh_pump_result_t::no_data);
        std::this_thread::sleep_for (1ms);
    }
    node.stop ();
    remote.close ();
}

void verify_fixed_drain_callback_barrier ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "drain-barrier");
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::runtime::mesh_node_host_service_t service ({registration}, serializers);
    const auto node = service.nodes ().front ();

    node->application_work_enqueued ();
    node->application_work_started ();
    service.seal_application_dispatch ();
    assert (!service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 20ms));

    std::thread completion ([node] {
        std::this_thread::sleep_for (20ms);
        node->application_work_finished ();
    });
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    completion.join ();
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);
}

void verify_local_and_wire_application_owner_keys_match ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    using runtime::host::owner_kind_t;
    runtime::host::ready_record_t local;
    assert (runtime::mesh::service_mailbox_t::application_owner (local)
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::node));
    local.owner_kind = owner_kind_t::channel;
    local.channel_name = "same";
    const auto channel = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (channel
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::channel, "same"));
    local.owner_kind = owner_kind_t::spot;
    local.spot_id = "same";
    const auto spot = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (spot == "spot:same");
    assert (spot
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::spot, "same"));
    local.owner_kind = owner_kind_t::actor;
    local.actor.emplace (actor_id_t ("same"), 1, "mesh", node_rid_t::from_string ("node"));
    const auto actor = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (actor
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::actor, "same"));
    assert (channel != spot && spot != actor && actor != channel);
}

void verify_owner_drain_continuation_during_executor_shutdown ()
{
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (1);
    std::promise<void> started;
    std::promise<void> release;
    auto release_wait = release.get_future ().share ();
    std::atomic_int completed{0};
    assert (executor.try_submit_internal ([&] {
        started.set_value ();
        release_wait.wait ();
        assert (executor.try_submit_internal ([&] { ++completed; }));
    }));
    started.get_future ().wait ();
    assert (!executor.drain_until (std::chrono::steady_clock::now ()));
    assert (!executor.try_submit ([] {}));
    release.set_value ();
    executor.drain ();
    assert (completed.load () == 1);
    assert (!executor.try_submit_internal ([] {}));
}

void verify_deferred_application_terminal_ownership ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;

    auto registration = make_node ("tcp://127.0.0.1:0", "deferred-terminal-owner");
    auto node = std::make_shared<detail::mesh_node_runtime_t> (registration);
    node->application_work_enqueued ();
    node->application_work_started ();

    auto stateful_completed = std::make_shared<std::atomic_bool> (false);
    std::atomic_int stateful_null_completions{0};
    std::atomic_int stateful_replies{0};
    std::atomic_int mailbox_releases{0};
    auto complete_stateful = [stateful_completed, &stateful_null_completions] {
        if (!stateful_completed->exchange (true, std::memory_order_acq_rel))
            ++stateful_null_completions;
    };
    auto late_reply = [stateful_completed, &stateful_replies] {
        if (stateful_completed->exchange (true, std::memory_order_acq_rel))
            return false;
        ++stateful_replies;
        return true;
    };
    auto terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
      node, complete_stateful, [&mailbox_releases] { ++mailbox_releases; });

    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 1);
    assert (late_reply ());
    assert (stateful_replies.load () == 1);
    assert (stateful_null_completions.load () == 0);

    terminal->settle ();
    terminal->settle ();
    terminal.reset ();
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);
    assert (mailbox_releases.load () == 1);
    assert (stateful_replies.load () == 1);
    assert (stateful_null_completions.load () == 0);

    // An inline terminal uses the same owner and therefore settles every
    // obligation once even when its destructor runs immediately afterwards.
    node->application_work_enqueued ();
    node->application_work_started ();
    auto inline_completed = std::make_shared<std::atomic_bool> (false);
    std::atomic_int inline_stateful{0};
    std::atomic_int inline_mailbox{0};
    {
        auto inline_terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
          node,
          [inline_completed, &inline_stateful] {
              if (!inline_completed->exchange (true, std::memory_order_acq_rel)) {
                  ++inline_stateful;
              }
          },
          [&inline_mailbox] { ++inline_mailbox; });
        inline_terminal->settle ();
    }
    assert (inline_stateful.load () == 1);
    assert (inline_mailbox.load () == 1);
    assert (node->active_application_callbacks () == 0);

    node->application_work_enqueued ();
    node->application_work_started ();
    std::atomic_int moved_mailbox{0};
    std::shared_ptr<runtime::application_dispatch_terminal_owner_t> deferred;
    {
        runtime::application_dispatch_terminal_owner_t inline_owner (
          node, {}, [&moved_mailbox] { ++moved_mailbox; });
        deferred = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
          std::move (inline_owner));
        inline_owner.settle ();
        assert (moved_mailbox.load () == 0);
    }
    assert (node->active_application_callbacks () == 1);
    deferred.reset ();
    assert (moved_mailbox.load () == 1);
    assert (node->active_application_callbacks () == 0);
}

#if defined(__unix__)
int run_cross_process_delivery ()
{
    const std::map<std::string, std::string> metadata_boundary{{"k", std::string (1018, 'v')}};
    const auto encoded_boundary =
      zlink::framework::detail::mesh_metadata_codec_t::encode (metadata_boundary);
    assert (encoded_boundary.size () == 1024);
    std::map<std::string, std::string> decoded_boundary;
    assert (
      zlink::framework::detail::mesh_metadata_codec_t::decode (encoded_boundary, decoded_boundary));
    assert (decoded_boundary == metadata_boundary);
    bool rejected_oversize = false;
    try {
        (void) zlink::framework::detail::mesh_metadata_codec_t::encode (
          {{"k", std::string (1019, 'v')}});
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_oversize = true;
    }
    assert (rejected_oversize);
    std::map<std::string, std::string> malformed_output;
    assert (!zlink::framework::detail::mesh_metadata_codec_t::decode ({1, 1, 1, 0xc0, 0, 0},
                                                                      malformed_output));
    assert (!zlink::framework::detail::mesh_metadata_codec_t::decode (
      {1, 2, 1, 'k', 0, 1, 'a', 1, 'k', 0, 1, 'b'}, malformed_output));

    int endpoint_pipe[2];
    int direct_ack_pipe[2];
    int channel_ack_pipe[2];
    int request_ack_pipe[2];
    int completion_ack_pipe[2];
    int spot_request_ack_pipe[2];
    int reciprocal_ready_pipe[2];
    int reciprocal_child_ready_pipe[2];
    int reciprocal_child_stop_pipe[2];
    int reciprocal_endpoint_pipe[2];
    int child_peer_pipe[2];
    int formal_descriptor_pipe[2];
    int formal_ack_pipe[2];
    if (pipe (endpoint_pipe) != 0 || pipe (direct_ack_pipe) != 0 || pipe (channel_ack_pipe) != 0
        || pipe (request_ack_pipe) != 0 || pipe (completion_ack_pipe) != 0
        || pipe (spot_request_ack_pipe) != 0 || pipe (reciprocal_ready_pipe) != 0
        || pipe (reciprocal_child_ready_pipe) != 0 || pipe (reciprocal_child_stop_pipe) != 0
        || pipe (reciprocal_endpoint_pipe) != 0 || pipe (child_peer_pipe) != 0
        || pipe (formal_descriptor_pipe) != 0 || pipe (formal_ack_pipe) != 0)
        return 1;

    const pid_t child = fork ();
    if (child < 0)
        return 1;
    if (child == 0) {
        close (endpoint_pipe[0]);
        close (direct_ack_pipe[0]);
        close (channel_ack_pipe[0]);
        close (request_ack_pipe[0]);
        close (completion_ack_pipe[1]);
        close (spot_request_ack_pipe[0]);
        close (reciprocal_ready_pipe[0]);
        close (reciprocal_child_ready_pipe[0]);
        close (reciprocal_child_ready_pipe[1]);
        close (reciprocal_child_stop_pipe[0]);
        close (reciprocal_child_stop_pipe[1]);
        close (reciprocal_endpoint_pipe[0]);
        close (reciprocal_endpoint_pipe[1]);
        close (child_peer_pipe[1]);
        close (formal_descriptor_pipe[0]);
        close (formal_ack_pipe[0]);
        auto state = make_node ("tcp://127.0.0.1:0", "vertical-b");
        zlink::framework::detail::mesh_node_runtime_t node (state);
        node.start ();
        auto target_spot = node.get_or_create_spot ("target-spot");
        auto target_actor = node.create_actor ("vertical.actor", "target-actor", {}, 5s);
        const std::uint64_t formal_descriptors[2]{target_spot.status ().lifecycle_generation (),
                                                  target_actor.ref ().object_generation ()};
        if (write (formal_descriptor_pipe[1], formal_descriptors, sizeof (formal_descriptors))
            != sizeof (formal_descriptors)) {
            _exit (8);
        }
        close (formal_descriptor_pipe[1]);
        const std::string endpoint = node.status ().local_endpoint ();
        const std::uint32_t size = static_cast<std::uint32_t> (endpoint.size ());
        if (write (endpoint_pipe[1], &size, sizeof (size)) != sizeof (size)
            || write (endpoint_pipe[1], endpoint.data (), endpoint.size ())
                 != static_cast<ssize_t> (endpoint.size ())) {
            _exit (2);
        }
        close (endpoint_pipe[1]);

        std::uint32_t peer_size = 0;
        if (read (child_peer_pipe[0], &peer_size, sizeof (peer_size)) != sizeof (peer_size))
            _exit (14);
        std::string peer_endpoint (peer_size, '\0');
        if (read (child_peer_pipe[0], peer_endpoint.data (), peer_endpoint.size ())
            != static_cast<ssize_t> (peer_endpoint.size ()))
            _exit (14);
        close (child_peer_pipe[0]);
        node.connect_peer (zlink::routing_id_t::from (std::string ("vertical-c")), peer_endpoint);

        const bool multi_peer_admitted = wait_until_admitted_count (node, 2);
        const char reciprocal_ready = multi_peer_admitted ? 1 : 0;
        (void) write (reciprocal_ready_pipe[1], &reciprocal_ready, sizeof (reciprocal_ready));
        close (reciprocal_ready_pipe[1]);

        const std::map<std::string, std::string> metadata{{"trace-id", "vertical-1"},
                                                          {"tenant", "sample"}};
        const auto encoded_metadata =
          zlink::framework::detail::mesh_metadata_codec_t::encode (metadata);
        const bool admitted = wait_until_admitted (node);
        const bool direct =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::node_send, "direct",
                       encoded_metadata);
        const char direct_ack = direct ? 1 : 0;
        (void) write (direct_ack_pipe[1], &direct_ack, sizeof (direct_ack));
        close (direct_ack_pipe[1]);
        const bool channel =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::channel_send,
                       "channel", encoded_metadata);
        const char channel_ack = channel ? 1 : 0;
        (void) write (channel_ack_pipe[1], &channel_ack, sizeof (channel_ack));
        close (channel_ack_pipe[1]);
        const bool request = reply_to_one_request (
          node, zlink::framework::runtime::host::record_kind_t::node_request, "request", "reply");
        const char request_ack = request ? 1 : 0;
        (void) write (request_ack_pipe[1], &request_ack, sizeof (request_ack));
        close (request_ack_pipe[1]);
        char completion_ack = 0;
        if (request)
            (void) read (completion_ack_pipe[0], &completion_ack, sizeof (completion_ack));
        close (completion_ack_pipe[0]);
        const bool spot =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::spot_send, "spot",
                       encoded_metadata);
        const char spot_ack = spot ? 1 : 0;
        (void) write (formal_ack_pipe[1], &spot_ack, sizeof (spot_ack));
        const bool spot_request =
          reply_to_one_request (node, zlink::framework::runtime::host::record_kind_t::spot_request,
                                "spot-request", "spot-reply");
        const char spot_request_ack = spot_request ? 1 : 0;
        (void) write (spot_request_ack_pipe[1], &spot_request_ack, sizeof (spot_request_ack));
        close (spot_request_ack_pipe[1]);
        const bool actor =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::actor_send, "actor",
                       encoded_metadata);
        const char actor_ack = actor ? 1 : 0;
        (void) write (formal_ack_pipe[1], &actor_ack, sizeof (actor_ack));
        close (formal_ack_pipe[1]);
        node.stop ();
        int exit_code = 0;
        if (!admitted)
            exit_code = 3;
        else if (!multi_peer_admitted)
            exit_code = 12;
        else if (!direct)
            exit_code = 4;
        else if (!channel)
            exit_code = 5;
        else if (!request)
            exit_code = 6;
        else if (completion_ack != 1)
            exit_code = 7;
        else if (!spot)
            exit_code = 9;
        else if (!spot_request)
            exit_code = 11;
        else if (!actor)
            exit_code = 10;
        _exit (exit_code);
    }

    close (endpoint_pipe[1]);
    close (direct_ack_pipe[1]);
    close (channel_ack_pipe[1]);
    close (request_ack_pipe[1]);
    close (completion_ack_pipe[0]);
    close (spot_request_ack_pipe[1]);
    close (reciprocal_ready_pipe[1]);
    close (child_peer_pipe[0]);
    close (formal_descriptor_pipe[1]);
    close (formal_ack_pipe[1]);
    std::uint32_t endpoint_size = 0;
    if (read (endpoint_pipe[0], &endpoint_size, sizeof (endpoint_size)) != sizeof (endpoint_size)) {
        return 1;
    }
    std::string endpoint (endpoint_size, '\0');
    if (read (endpoint_pipe[0], endpoint.data (), endpoint.size ())
        != static_cast<ssize_t> (endpoint.size ())) {
        return 1;
    }
    close (endpoint_pipe[0]);
    std::uint64_t formal_descriptors[2]{};
    assert (read (formal_descriptor_pipe[0], formal_descriptors, sizeof (formal_descriptors))
            == sizeof (formal_descriptors));
    close (formal_descriptor_pipe[0]);

    const pid_t reciprocal_child = fork ();
    assert (reciprocal_child >= 0);
    if (reciprocal_child == 0) {
        close (child_peer_pipe[1]);
        close (reciprocal_endpoint_pipe[0]);
        close (reciprocal_child_ready_pipe[0]);
        close (reciprocal_child_stop_pipe[1]);
        auto reciprocal_state = make_node ("tcp://127.0.0.1:0", "vertical-c");
        reciprocal_state->peer_connections.push_back (zlink::framework::mesh_peer_connection_t{
          1, zlink::routing_id_t::from (std::string ("vertical-b")), endpoint});
        zlink::framework::detail::mesh_node_runtime_t reciprocal_node (reciprocal_state);
        reciprocal_node.start ();
        const auto reciprocal_endpoint = reciprocal_node.status ().local_endpoint ();
        const auto reciprocal_size = static_cast<std::uint32_t> (reciprocal_endpoint.size ());
        (void) write (reciprocal_endpoint_pipe[1], &reciprocal_size, sizeof (reciprocal_size));
        (void) write (reciprocal_endpoint_pipe[1], reciprocal_endpoint.data (),
                      reciprocal_endpoint.size ());
        close (reciprocal_endpoint_pipe[1]);
        const char ready = wait_until_admitted (reciprocal_node) ? 1 : 0;
        (void) write (reciprocal_child_ready_pipe[1], &ready, sizeof (ready));
        close (reciprocal_child_ready_pipe[1]);
        char stop = 0;
        (void) read (reciprocal_child_stop_pipe[0], &stop, sizeof (stop));
        close (reciprocal_child_stop_pipe[0]);
        reciprocal_node.stop ();
        _exit (ready == 1 && stop == 1 ? 0 : 13);
    }
    close (reciprocal_endpoint_pipe[1]);
    close (reciprocal_child_ready_pipe[1]);
    close (reciprocal_child_stop_pipe[0]);
    std::uint32_t reciprocal_size = 0;
    assert (read (reciprocal_endpoint_pipe[0], &reciprocal_size, sizeof (reciprocal_size))
            == sizeof (reciprocal_size));
    std::string reciprocal_endpoint (reciprocal_size, '\0');
    assert (
      read (reciprocal_endpoint_pipe[0], reciprocal_endpoint.data (), reciprocal_endpoint.size ())
      == static_cast<ssize_t> (reciprocal_endpoint.size ()));
    close (reciprocal_endpoint_pipe[0]);
    assert (write (child_peer_pipe[1], &reciprocal_size, sizeof (reciprocal_size))
            == sizeof (reciprocal_size));
    assert (write (child_peer_pipe[1], reciprocal_endpoint.data (), reciprocal_endpoint.size ())
            == static_cast<ssize_t> (reciprocal_endpoint.size ()));
    close (child_peer_pipe[1]);
    char reciprocal_child_ready = 0;
    assert (read (reciprocal_child_ready_pipe[0], &reciprocal_child_ready,
                  sizeof (reciprocal_child_ready))
            == sizeof (reciprocal_child_ready));
    close (reciprocal_child_ready_pipe[0]);
    assert (reciprocal_child_ready == 1);

    auto state = make_node ("tcp://127.0.0.1:*", "vertical-a");
    state->peer_connections.push_back (zlink::framework::mesh_peer_connection_t{1, {}, endpoint});
    zlink::framework::detail::mesh_node_runtime_t node (state);
    configure_vertical_route_fences (node);
    node.start ();
    assert (wait_until_admitted (node));
    char reciprocal_ready = 0;
    assert (read (reciprocal_ready_pipe[0], &reciprocal_ready, sizeof (reciprocal_ready))
            == sizeof (reciprocal_ready));
    close (reciprocal_ready_pipe[0]);
    assert (reciprocal_ready == 1);

    const std::map<std::string, std::string> metadata{{"trace-id", "vertical-1"},
                                                      {"tenant", "sample"}};
    const std::vector<zlink::message_t> direct_parts{
      zlink::message_t::from (std::string ("direct"))};
    assert (submit_until_ok ([&] {
        return node.send_to_node (zlink::routing_id_t::from (std::string ("vertical-b")),
                                  direct_parts, metadata);
    }));
    char direct_ack = 0;
    assert (read (direct_ack_pipe[0], &direct_ack, sizeof (direct_ack)) == sizeof (direct_ack));
    close (direct_ack_pipe[0]);
    assert (direct_ack == 1);
    const std::vector<zlink::message_t> channel_parts{
      zlink::message_t::from (std::string ("channel"))};
    assert (
      submit_until_ok ([&] { return node.send_to_channel ("work", channel_parts, metadata); }));
    char channel_ack = 0;
    assert (read (channel_ack_pipe[0], &channel_ack, sizeof (channel_ack)) == sizeof (channel_ack));
    close (channel_ack_pipe[0]);
    assert (channel_ack == 1);
    const std::vector<zlink::message_t> request_parts{
      zlink::message_t::from (std::string ("request"))};
    zlink::framework::runtime::host::pending_operation_t operation_id;
    assert (std::move (node.request_to_node (zlink::routing_id_t::from (std::string ("vertical-b")),
                                             request_parts, operation_id, 5s, metadata))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    char request_ack = 0;
    assert (read (request_ack_pipe[0], &request_ack, sizeof (request_ack)) == sizeof (request_ack));
    close (request_ack_pipe[0]);
    assert (request_ack == 1);
    assert (receive_completion (node, operation_id, "reply"));
    const char completion_ack = 1;
    assert (write (completion_ack_pipe[1], &completion_ack, sizeof (completion_ack))
            == sizeof (completion_ack));
    close (completion_ack_pipe[1]);
    const std::vector<zlink::message_t> spot_parts{zlink::message_t::from (std::string ("spot"))};
    assert (submit_until_ok ([&] {
        return node.send_to_spot (
          "source-spot", zlink::routing_id_t::from (std::string ("vertical-b")), "target-spot",
          formal_descriptors[0], spot_parts,
          zlink::framework::detail::mesh_metadata_codec_t::encode (metadata));
    }));
    char spot_ack = 0;
    assert (read (formal_ack_pipe[0], &spot_ack, sizeof (spot_ack)) == sizeof (spot_ack));
    assert (spot_ack == 1);
    const std::vector<zlink::message_t> spot_request_parts{
      zlink::message_t::from (std::string ("spot-request"))};
    zlink::framework::runtime::host::pending_operation_t spot_operation_id;
    assert (
      std::move (node.request_to_spot (
                   "source-spot", zlink::routing_id_t::from (std::string ("vertical-b")),
                   "target-spot", formal_descriptors[0], spot_request_parts, spot_operation_id, 5s,
                   zlink::framework::detail::mesh_metadata_codec_t::encode (metadata)))
        .result ()
        .value ()
      == zlink::submit_result_t::ok);
    char spot_request_ack = 0;
    assert (read (spot_request_ack_pipe[0], &spot_request_ack, sizeof (spot_request_ack))
            == sizeof (spot_request_ack));
    close (spot_request_ack_pipe[0]);
    assert (spot_request_ack == 1);
    assert (receive_completion (node, spot_operation_id, "spot-reply"));
    const std::vector<zlink::message_t> actor_parts{zlink::message_t::from (std::string ("actor"))};
    assert (submit_until_ok ([&] {
        return node.send_to_actor (
          zlink::framework::runtime::host::public_host_runtime_t::remote_actor_ref (
            zlink::routing_id_t::from (std::string ("vertical-b")), "target-actor",
            formal_descriptors[1]),
          actor_parts, zlink::framework::detail::mesh_metadata_codec_t::encode (metadata), 1, 1);
    }));
    char actor_ack = 0;
    assert (read (formal_ack_pipe[0], &actor_ack, sizeof (actor_ack)) == sizeof (actor_ack));
    close (formal_ack_pipe[0]);
    assert (actor_ack == 1);

    auto local_source = node.get_or_create_spot ("source-spot");
    auto local_target = node.get_or_create_spot ("local-target");
    const std::vector<zlink::message_t> local_request_parts{
      zlink::message_t::from (std::string ("local-request"))};
    const auto local_node_rid = node.status ().routing_id ();
    const auto local_target_generation = local_target.status ().lifecycle_generation ();
    const std::vector<zlink::message_t> local_send_parts{
      zlink::message_t::from (std::string ("local-send"))};
    assert (std::move (local_source.send_to_spot (local_node_rid, "local-target",
                                                  local_target_generation, local_send_parts))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    bool local_send_delivered = false;
    (void) std::move (
      node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &owner,
                                const zlink::framework::runtime::host::receive_record_t &record,
                                std::vector<zlink::message_t> parts) {
          local_send_delivered =
            owner.owner_kind == zlink::framework::runtime::host::owner_kind_t::spot
            && owner.spot_id == "local-target"
            && record.kind == zlink::framework::runtime::host::record_kind_t::spot_send
            && !parts.empty () && parts.front ().to_string () == "local-send";
      }))
      .result ()
      .value ();
    assert (local_send_delivered);

    zlink::framework::runtime::host::pending_operation_t local_timeout_operation;
    int local_timeout_callbacks = 0;
    std::promise<void> local_timeout_delivered;
    auto local_timeout_ready = local_timeout_delivered.get_future ();
    assert (std::move (
              local_source.request_to_spot (
                local_node_rid, "local-target", local_target_generation, local_request_parts,
                local_timeout_operation, zlink::send_flags_t::none, 25ms, {},
                [&] (zlink::framework::runtime::foundation::operation_terminal_t terminal,
                     zlink::framework::result_t<std::vector<zlink::message_t>>) {
                    if (terminal
                        == zlink::framework::runtime::foundation::operation_terminal_t::timed_out)
                        ++local_timeout_callbacks;
                    local_timeout_delivered.set_value ();
                }))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    std::this_thread::sleep_for (40ms);
    (void) std::move (
      node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                               const zlink::framework::runtime::host::receive_record_t &,
                               std::vector<zlink::message_t>) {}))
      .result ()
      .value ();
    // Expiry claims the entry in this ingress turn; the already-reserved
    // dispatcher delivers the callback in a new turn.
    assert (local_timeout_ready.wait_for (1s) == std::future_status::ready);
    assert (local_timeout_callbacks == 1);

    zlink::framework::runtime::host::pending_operation_t local_shutdown_operation;
    int local_shutdown_callbacks = 0;
    assert (
      std::move (local_source.request_to_spot (
                   local_node_rid, "local-target", local_target_generation, local_request_parts,
                   local_shutdown_operation, zlink::send_flags_t::none, 5s, {},
                   [&] (zlink::framework::runtime::foundation::operation_terminal_t terminal,
                        zlink::framework::result_t<std::vector<zlink::message_t>>) {
                       if (terminal
                           == zlink::framework::runtime::foundation::operation_terminal_t::shutdown)
                           ++local_shutdown_callbacks;
                   }))
        .result ()
        .value ()
      == zlink::submit_result_t::ok);

    zlink::framework::runtime::host::pending_operation_t callbackless_shutdown_operation;
    assert (std::move (local_source.request_to_spot (local_node_rid, "local-target",
                                                     local_target_generation, local_request_parts,
                                                     callbackless_shutdown_operation,
                                                     zlink::send_flags_t::none, 5s, {}))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    auto callbackless_wait = std::async (std::launch::async, [&] {
        return node.wait_for_completion (callbackless_shutdown_operation, 5s);
    });

    auto race_state = make_node ("tcp://127.0.0.1:*", "local-race");
    zlink::framework::detail::mesh_node_runtime_t race_node (race_state);
    configure_vertical_route_fences (race_node);
    race_node.start ();
    auto race_source = race_node.get_or_create_spot ("race-source");
    auto race_target = race_node.get_or_create_spot ("race-target");
    const auto race_node_rid = race_node.status ().routing_id ();
    const auto race_target_generation = race_target.status ().lifecycle_generation ();
    const std::vector<zlink::message_t> race_parts{zlink::message_t::from (std::string ("race"))};
    std::atomic_bool submit_race{true};
    std::atomic_int accepted_race_requests{0};
    std::atomic_int terminal_race_callbacks{0};
    std::thread race_submitter ([&] {
        while (submit_race.load (std::memory_order_acquire)) {
            zlink::framework::runtime::host::pending_operation_t operation;
            const auto submitted =
              std::move (race_source.request_to_spot (
                           race_node_rid, "race-target", race_target_generation, race_parts,
                           operation, zlink::send_flags_t::none, 1s, {},
                           [&] (zlink::framework::runtime::foundation::operation_terminal_t,
                                zlink::framework::result_t<std::vector<zlink::message_t>>) {
                               terminal_race_callbacks.fetch_add (1, std::memory_order_relaxed);
                           }))
                .result ()
                .value ();
            if (submitted == zlink::submit_result_t::ok) {
                accepted_race_requests.fetch_add (1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for (1ms);
            }
        }
    });
    std::this_thread::sleep_for (20ms);
    race_node.stop ();
    submit_race.store (false, std::memory_order_release);
    race_submitter.join ();
    assert (accepted_race_requests.load (std::memory_order_relaxed)
            == terminal_race_callbacks.load (std::memory_order_relaxed));

    int status = 0;
    waitpid (child, &status, 0);
    node.stop ();
    const auto callbackless_result = callbackless_wait.get ();
    assert (!callbackless_result);
    assert (callbackless_result.error () != nullptr);
    assert (zlink::framework::detail::boundary_state (*callbackless_result.error ())
            == zlink::framework::detail::boundary_error_t::shutdown);
    assert (local_shutdown_callbacks == 1);
    const char reciprocal_stop = 1;
    assert (write (reciprocal_child_stop_pipe[1], &reciprocal_stop, sizeof (reciprocal_stop))
            == sizeof (reciprocal_stop));
    close (reciprocal_child_stop_pipe[1]);
    int reciprocal_status = 0;
    waitpid (reciprocal_child, &reciprocal_status, 0);
    assert (WIFEXITED (reciprocal_status));
    assert (WEXITSTATUS (reciprocal_status) == 0);
    return WIFEXITED (status) ? WEXITSTATUS (status) : 4;
}
#endif
} // namespace

int main (int argc, char **argv)
{
#if defined(__unix__)
    if (argc == 2 && std::string_view (argv[1]) == "--cross-process")
        return run_cross_process_delivery ();
#endif
    verify_local_join_timeout_releases_membership ();
    verify_unselected_object_role_defaults_to_none ();
    verify_automatic_identity_and_port_builder ();
    verify_public_runtime_surface ();
    verify_slow_observer_does_not_block_stop ();
    verify_object_client_registration_boundary ();
    verify_host_shutdown_seal_reaches_raw_mesh ();
    verify_fixed_drain_callback_barrier ();
    verify_local_and_wire_application_owner_keys_match ();
    verify_owner_drain_continuation_during_executor_shutdown ();
    verify_deferred_application_terminal_ownership ();
    verify_server_descriptor_publishes_framework_entry_spot_without_application_entry ();
    verify_descriptor_retire_order_and_pre_seal_rollback ();
    verify_local_node_submit_bridge ();
    verify_route_internal_packets_precede_application_dispatch ();
    verify_direct_target_falls_through_absent_location_store_entry ();
    verify_request_to_never_admitted_target_reports_not_found ();
#if defined(__unix__)
    // Fork the delivery peers before any runtime threads exist in their parent.
    // A fork after the unit fixtures inherits dispatcher state, but not its worker.
    const auto delivery = fork ();
    assert (delivery >= 0);
    if (delivery == 0) {
        execl (argv[0], argv[0], "--cross-process", static_cast<char *> (nullptr));
        _exit (127);
    }
    int delivery_status = 0;
    assert (waitpid (delivery, &delivery_status, 0) == delivery);
    if (!WIFEXITED (delivery_status) || WEXITSTATUS (delivery_status) != 0)
        return WIFEXITED (delivery_status) ? WEXITSTATUS (delivery_status) : 4;
#else
    // Cross-process metadata and delivery coverage remains POSIX-only because
    // the repository has no Windows C++ unit-test child-process harness.  A
    // different in-process smoke check must not stand in for that scenario.
#endif
    verify_actor_route_resolver_preserves_unavailable ();
    return 0;
}
