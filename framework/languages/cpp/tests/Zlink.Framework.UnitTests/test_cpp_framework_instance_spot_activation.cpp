/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include "runtime_context_hooks_environment.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

class reserve_loser_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority (zlink::framework::authority_key_t, std::stop_token = {}) override
    {
        ++reads;
        co_return zlink::framework::authority_missing_t{std::chrono::system_clock::now ()};
    }

    zlink::framework::task_t<zlink::framework::object_reserve_result_t>
    reserve (zlink::framework::object_reserve_request_t request, std::stop_token = {}) override
    {
        ++reserves;
        observed = std::move (request);
        captured = zlink::framework::runtime::protocol::decode_instance_activation_recovery (
          relocations->get (observed->intent.request_content_reference).value (), false);
        if (expire_deadline)
            std::this_thread::sleep_until (observed->operation_deadline);
        co_return zlink::framework::object_reserve_conflict_t{
          zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}};
    }

    int reads = 0;
    int reserves = 0;
    bool expire_deadline = false;
    std::optional<zlink::framework::object_reserve_request_t> observed;
    std::shared_ptr<zlink::framework::runtime::stateful::relocation_store_port_t> relocations;
    std::optional<zlink::framework::runtime::protocol::instance_activation_recovery_t> captured;
};

struct event_t
{
    static constexpr const char *packet_name = "instance.event";
    int value{};
};

static void verify_same_target_activation_order (bool ready_request,
                                                 bool retransmission_mismatch = false)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    class held_reservation_t final : public rt::in_memory_location_repository_t
    {
      public:
        fw::task_t<fw::object_reserve_result_t> reserve (fw::object_reserve_request_t request,
                                                         std::stop_token stop = {}) override
        {
            ++reserves;
            if (!hold_commit)
                co_await release.task ();
            co_return co_await rt::in_memory_location_repository_t::reserve (std::move (request),
                                                                             stop);
        }
        fw::task_t<fw::object_commit_result_t>
        commit (fw::object_commit_request_t request,
                std::stop_token stop = {},
                std::chrono::system_clock::time_point deadline = {}) override
        {
            auto result = co_await rt::in_memory_location_repository_t::commit (std::move (request),
                                                                                stop, deadline);
            ++commits;
            if (hold_commit)
                co_await release.task ();
            co_return result;
        }
        bool hold_commit = false;
        std::atomic_int reserves{0};
        std::atomic_int commits{0};
        fw::task_completion_source_t<void> release;
    };
    auto store = std::make_shared<held_reservation_t> ();
    store->hold_commit = ready_request;
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "same-target";
    descriptor.rid = zlink::routing_id_t::from ("target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7101";
    descriptor.security_identity = "test-owner";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::instance_spot,
                                               "room-type", fw::maintenance_policy_kind_t::disabled,
                                               false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "room-type", {0, 0, 1}});
    ASSERT_EQ (fw::location_write_status_t::stored,
               store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
    auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
    auto relocations = std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
    auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "same-target",
                              .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"}}});
    std::vector<std::uint64_t> queued;
    target->configure_instance_spot_operations (
      store, relocations, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [] (const auto &, const auto &) { return true; },
        [&] (auto command, auto, auto) -> fw::task_t<host::instance_spot_activation_result_t> {
            queued.push_back (command->activation.operation.low);
            co_return host::instance_spot_activation_result_t{
              0, 0,
              command->activation.request
                ? std::optional<rt::protocol::application_payload_t> (
                    rt::protocol::application_payload_t{"reply", "application/json", {'1'}})
                : std::nullopt};
        }});
    target->start ();
    struct cleanup_t
    {
        std::function<void ()> finish;
        ~cleanup_t () { finish (); }
    };
    const cleanup_t cleanup{[&] {
        store->release.complete (fw::result_t<void>::success ());
        target->close ();
    }};
    const auto deadline =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    (std::chrono::system_clock::now () + 3s).time_since_epoch ())
                                    .count ());
    rt::protocol::instance_spot_activation_header_t event{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "room", "same-target", "room-type", "1", deadline},
      target->status ().lifecycle_generation (),
      target->status ().routing_id ().to_bytes (),
      std::nullopt,
      false,
      {1571, 1},
      0,
      false};
    const rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    ASSERT_TRUE (target
                   ->send_instance_spot_activation_remote (target->status ().routing_id (), event,
                                                           std::nullopt, payload)
                   .result ());
    auto pump = [&] {
        ASSERT_TRUE (
          target->dispatch_ready ([] (const auto &, const auto &, auto) {}, false).result ());
    };
    const auto until = std::chrono::steady_clock::now () + 3s;
    while ((store->reserves == 0 || (ready_request && store->commits == 0))
           && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (1, store->reserves);
    if (ready_request)
        ASSERT_EQ (1, store->commits);
    auto request = event;
    request.request = true;
    request.operation.low = retransmission_mismatch ? 1 : 2;
    request.reply_route_id = 2;
    if (retransmission_mismatch)
        ++request.target.deadline_unix_ms;
    if (ready_request) {
        const auto authority =
          store->read_authority (rt::spot_authority_key ("room")).result ().value ();
        const auto &snapshot = std::get<fw::authority_snapshot_t> (authority);
        request.target.object_generation = snapshot.object_generation;
        request.target.authority_owner_generation = snapshot.authority_owner_generation;
        request.target.owner_id = snapshot.owner.owner_id;
        request.target.owner_lease_generation = snapshot.owner.lease_generation;
        request.target.store_version = snapshot.store_version;
        request.target.instance_intent = true;
    }
    std::promise<rt::protocol::reply_header_t> reply;
    auto result = reply.get_future ();
    ASSERT_TRUE (target
                   ->activate_instance_spot_remote (
                     target->status ().routing_id (), request, std::nullopt, payload, 3s,
                     [&] (auto, auto header, auto) { reply.set_value (header); })
                   .result ());
    pump ();
    EXPECT_EQ (1, store->reserves);
    EXPECT_TRUE (queued.empty ());
    store->release.complete (fw::result_t<void>::success ());
    while (result.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (std::future_status::ready, result.wait_for (0ms));
    EXPECT_EQ (retransmission_mismatch
                 ? static_cast<std::uint32_t> (rt::protocol::request_terminal_result::protocolError)
                 : 0u,
               result.get ().terminal_result);
    EXPECT_EQ (retransmission_mismatch ? std::vector<std::uint64_t>{1}
                                       : (std::vector<std::uint64_t>{1, 2}),
               queued);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      SameTargetColdOperationsReserveOnceAndQueueInArrivalOrder)
{
    verify_same_target_activation_order (false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, SameTargetRetransmissionMismatchIsRejectedBeforeJoin)
{
    verify_same_target_activation_order (false, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ReadyOperationJoinsBeforeDurableFirstRecordAdmission)
{
    verify_same_target_activation_order (true);
}

struct request_t
{
    static constexpr const char *packet_name = "instance.request";
    int value{};
};

struct reply_t
{
    int value{};
};

struct traced_event_t
{
    static constexpr const char *packet_name = "instance.traced.event";
    int value{};
};

struct traced_request_t
{
    static constexpr const char *packet_name = "instance.traced.request";
    int value{};
};

struct traced_reply_t
{
    int value{};
};

template <typename T>
    requires (std::is_same_v<T, event_t> || std::is_same_v<T, request_t>
              || std::is_same_v<T, reply_t> || std::is_same_v<T, traced_event_t>
              || std::is_same_v<T, traced_request_t> || std::is_same_v<T, traced_reply_t>)
void to_json (nlohmann::json &json, const T &value)
{
    json = value.value;
}

template <typename T>
    requires (std::is_same_v<T, event_t> || std::is_same_v<T, request_t>
              || std::is_same_v<T, reply_t> || std::is_same_v<T, traced_event_t>
              || std::is_same_v<T, traced_request_t> || std::is_same_v<T, traced_reply_t>)
void from_json (const nlohmann::json &json, T &value)
{
    value.value = json.get<int> ();
}

class resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        ++reads;
        if (lookup_gate)
            co_await lookup_gate->task ();
        const auto found = addresses.find (spot_id);
        co_return found == addresses.end ()
          ? std::nullopt
          : std::optional<zlink::framework::runtime::spot_address_t> (found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override { addresses.clear (); }

    std::atomic_int reads{0};
    std::shared_ptr<zlink::framework::task_completion_source_t<void>> lookup_gate;
    std::map<std::string, zlink::framework::runtime::spot_address_t> addresses;
};

class traced_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit traced_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&traced_instance_spot_t::on_event> ();
        _context.handlers ().add_handler<&traced_instance_spot_t::on_request> ();
    }

    void on_event (const traced_event_t &event) { last_event = event.value; }

    traced_reply_t on_request (const traced_request_t &request) { return {request.value + 1}; }

    int last_event = 0;

  private:
    zlink::framework::instance_spot_context_t _context;
};

class close_after_reply_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    close_after_reply_instance_spot_t (
      zlink::framework::instance_spot_context_t context,
      std::shared_ptr<std::optional<zlink::framework::task_t<bool>>> close_result) :
        _context (std::move (context)), _close_result (std::move (close_result))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&close_after_reply_instance_spot_t::on_request> ();
    }

    zlink::framework::task_t<reply_t> on_request (const request_t &request)
    {
        _close_result->emplace (_context.close ());
        co_return reply_t{request.value + 1};
    }

  private:
    zlink::framework::instance_spot_context_t _context;
    std::shared_ptr<std::optional<zlink::framework::task_t<bool>>> _close_result;
};

TEST (ZLinkFrameworkInstanceSpotActivation, CloseFromRequestReturnsReplyBeforeCloseCompletes)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto close_result = std::make_shared<std::optional<zlink::framework::task_t<bool>>> ();
    builder.add_route_mesh ("instance-close-after-reply")
      .add_instance_spot_factory<close_after_reply_instance_spot_t> (
        "closing-player",
        [close_result] (zlink::framework::instance_spot_context_t context) {
            return std::make_shared<close_after_reply_instance_spot_t> (std::move (context),
                                                                        close_result);
        },
        [] (auto &factory) { factory.disable_relocation (); });
    auto runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder, "instance-close-after-reply");
    ASSERT_TRUE (runtime);
    zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
      .bind_serializers (serializers);

    const auto spot_id = zlink::framework::spot_id_t ("closing-player-1");
    const auto created = runtime->get_or_create_spot ("closing-player", spot_id);
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const auto payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<request_t> ().serialize (request_t{41}));
    std::function<void ()> accepted_turn_terminal;
    const auto first =
      runtime
        ->dispatch_instance_activation (
          spot_id, request_t::packet_name, serializers.get<request_t> ().content_type (),
          payload.to_bytes (), {}, true, "close-request-1", provider, serializers, std::nullopt,
          std::nullopt, &accepted_turn_terminal)
        .result ();
    ASSERT_TRUE (first) << (first.error () ? first.error ()->what () : "unknown error");
    ASSERT_TRUE (accepted_turn_terminal);
    const auto reply = serializers.get<reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (first.value ()));
    EXPECT_EQ (42, reply.value);
    accepted_turn_terminal ();
    ASSERT_TRUE (close_result->has_value ());
    const auto closed = close_result->value ().result ();
    ASSERT_TRUE (closed);
    EXPECT_TRUE (closed.value ());

    const auto second =
      runtime
        ->dispatch_instance_activation (
          spot_id, request_t::packet_name, serializers.get<request_t> ().content_type (),
          payload.to_bytes (), {}, true, "close-request-2", provider, serializers)
        .result ();
    ASSERT_FALSE (second);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found, second.error_kind ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIntentActivatesOnceAndReadyOwnerIgnoresPlacementHints)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::atomic_int activations{0};
    std::atomic_int ready_requests{0};
    runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::optional<zlink::framework::runtime::spot_address_t> &cached_route,
           const std::string &, std::type_index,
           std::function<zlink::framework::serialized_payload_t (
             zlink::framework::serializer_registry_t &)>,
           const std::map<std::string, std::string> &, std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-17", std::string (spot_id));
          EXPECT_FALSE (cached_route);
          EXPECT_EQ (std::optional<std::string> ("commerce"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("shopping-cart"), intent.stable_type);
          ++activations;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          resolver.addresses.insert_or_assign ("cart-17", address);
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const auto &, const auto &, const auto &cached_route, std::string, std::type_index, auto,
           std::chrono::milliseconds, auto) {
          EXPECT_TRUE (cached_route);
          EXPECT_EQ ("commerce", cached_route->mesh_name);
          EXPECT_EQ ("cart-node", cached_route->node_rid.to_string ());
          ++ready_requests;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (
                serializers.get<reply_t> ().serialize (reply_t{71}))));
      });

    std::atomic_int sends{0};
    std::atomic_int requests{0};
    zlink::framework::runtime::messaging::envelope_codec_t envelopes;
    runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &node, const std::string &spot, std::uint64_t generation,
           zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-node", node.to_string ());
          EXPECT_EQ ("cart-17", spot);
          EXPECT_EQ (1u, generation);
          ++sends;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          ++requests;
          auto header = envelopes.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{71};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelopes.encode_parts (header, reply, serializers));
      });

    auto client = builder.route_client (serializers);
    const auto sent = client.send_to_spot ("cart-17", event_t{1})
                        .instance_spot ("shopping-cart")
                        .in_mesh ("commerce")
                        .async ()
                        .result ();
    ASSERT_TRUE (sent);

    const auto reply = client.request_to_spot ("cart-17", request_t{2})
                         .instance_spot ("different-type")
                         .in_mesh ("different-mesh")
                         .async<reply_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (71, reply.value ().value);
    EXPECT_EQ (1, activations.load ());
    EXPECT_EQ (0, sends.load ());
    EXPECT_EQ (0, requests.load ());
    EXPECT_EQ (1, ready_requests.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, StoreReadFailureHasUnavailablePublicKind)
{
    namespace fw = zlink::framework;
    class failing_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            ++reads;
            throw std::runtime_error ("private provider command/key details");
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::logic_error ("unexpected Store write");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::logic_error ("unexpected Store scan");
        }
        int reads = 0;
    } store;
    fw::runtime::provider_location_repository_t repository (store);
    fw::runtime::store_location_resolvers_t resolver (repository);
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_spot_address_resolver (resolver);
    auto client = builder.route_client (serializers);
    const auto send = client.send_to_spot ("uncached-spot", event_t{1}).async ().result ();
    EXPECT_FALSE (send.has_value ());
    EXPECT_NE (nullptr, send.error ());
    if (send.error ()) {
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, send.error_kind ());
        EXPECT_EQ (std::string::npos, std::string (send.error ()->what ()).find ("command"));
    }
    const auto request = client.request_to_spot ("uncached-spot", request_t{1})
                           .timeout (std::chrono::minutes (1))
                           .async<reply_t> ()
                           .result ();
    EXPECT_FALSE (request.has_value ());
    EXPECT_NE (nullptr, request.error ());
    if (request.error ()) {
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, request.error_kind ());
        EXPECT_EQ (std::string::npos, std::string (request.error ()->what ()).find ("command"));
    }
    EXPECT_EQ (2, store.reads);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RepositoryClassifiesEveryProviderOperation)
{
    namespace fw = zlink::framework;
    class fault_store_t final : public fw::location_store_t
    {
      public:
        explicit fault_store_t (std::string operation) : operation (std::move (operation)) {}
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            if (operation == "read")
                throw std::runtime_error ("private provider failure");
            return fw::task_t<fw::store_read_result_t> (
              fw::result_t<fw::store_read_result_t>::success (
                fw::store_read_result_t{fw::store_missing_t{std::chrono::system_clock::now ()}}));
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::runtime_error ("private provider failure");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::runtime_error ("private provider failure");
        }
        std::string operation;
    };
    const auto verify = [] (const auto &result) {
        ASSERT_FALSE (result.has_value ());
        ASSERT_NE (nullptr, result.error ());
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, result.error_kind ());
        try {
            std::rethrow_exception (result.exception ());
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (std::string::npos, std::string (error.what ()).find ("private"));
            try {
                std::rethrow_if_nested (error);
                FAIL () << "provider cause was lost";
            }
            catch (const std::runtime_error &cause) {
                EXPECT_STREQ ("private provider failure", cause.what ());
            }
        }
    };
    fault_store_t read_store ("read");
    fw::runtime::provider_location_repository_t read_repository (read_store);
    verify (read_repository.read_owner_lease ("owner").result ());
    fault_store_t write_store ("write");
    fw::runtime::provider_location_repository_t write_repository (write_store);
    verify (write_repository.claim_owner_lease ("owner", std::chrono::seconds (30)).result ());
    fault_store_t scan_store ("scan");
    fw::runtime::provider_location_repository_t scan_repository (scan_store);
    verify (scan_repository.list_mesh_nodes ("mesh").result ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, RepositoryPreservesExcludedProviderFailures)
{
    namespace fw = zlink::framework;
    class fault_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t) override
        {
            std::rethrow_exception (failure);
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t) override
        {
            throw std::logic_error ("unexpected write");
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t) override
        {
            throw std::logic_error ("unexpected scan");
        }
        std::exception_ptr failure;
    } store;
    fw::runtime::provider_location_repository_t repository (store);
    for (const auto &failure :
         {std::make_exception_ptr (fw::framework_exception_t (
            fw::framework_error_kind_t::protocol_error, "typed failure")),
          std::make_exception_ptr (std::invalid_argument ("caller validation")),
          std::make_exception_ptr (std::out_of_range ("caller range")),
          std::make_exception_ptr (
            std::system_error (std::make_error_code (std::errc::operation_canceled)))}) {
        store.failure = failure;
        const auto result = repository.read_owner_lease ("owner").result ();
        EXPECT_FALSE (result.has_value ());
        EXPECT_EQ (failure, result.exception ());
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, MissingWithoutIntentDoesNotActivate)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure, "must not activate");
      },
      [&] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          ++activations;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, "must not activate"));
      });

    const auto result =
      builder.route_client (serializers).send_to_spot ("missing", event_t{1}).async ().result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found, result.error_kind ());
    EXPECT_EQ (0, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, ActivationKeepsSubmissionStartAcrossRouteLookup)
{
    namespace fw = zlink::framework;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    resolver.lookup_gate = std::make_shared<fw::task_completion_source_t<void>> ();
    runtime.bind_spot_address_resolver (resolver);
    std::optional<std::chrono::system_clock::time_point> observed_start;
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point started_at) -> fw::task_t<fw::result_t<void>> {
          observed_start = started_at;
          co_return fw::result_t<void>::success ();
      },
      [] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          return fw::task_t<zlink::message_t> (fw::result_t<zlink::message_t>::failure (
            fw::framework_error_kind_t::internal_failure, "unused request activation"));
      });
    const auto before = std::chrono::system_clock::now ();
    auto send = builder.route_client (serializers)
                  .send_to_spot ("cold", event_t{1})
                  .instance_spot ("traced-player")
                  .async ();
    const auto lookup_started = std::chrono::system_clock::now ();
    EXPECT_EQ (1, resolver.reads.load ());
    EXPECT_FALSE (send.await_ready ());
    EXPECT_FALSE (observed_start);
    resolver.lookup_gate->complete (fw::result_t<void>::success ());
    ASSERT_TRUE (send.result ());
    ASSERT_TRUE (observed_start);
    EXPECT_GE (*observed_start, before);
    EXPECT_LE (*observed_start, lookup_started);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      OneWayColdActivationUsesSourceRequestBudgetWithoutEndingSend)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    auto &options = app.add_zlink_framework ();
    class lookup_gate_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
        {
            if (key.value.find ("budget-player-1") != std::string::npos && !held.exchange (true)) {
                entered.set_value ();
                co_await release.task ();
            }
            co_return co_await inner.read (std::move (key));
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
        {
            return inner.scan (std::move (request));
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
        {
            return inner.write (std::move (request));
        }
        fw::runtime::in_memory_location_store_t inner;
        std::atomic_bool held{false};
        std::promise<void> entered;
        fw::task_completion_source_t<void> release;
    };
    auto store = std::make_shared<lookup_gate_store_t> ();
    auto lookup_entered = store->entered.get_future ();
    options.add_location_store (store);
    options.add_relocation_store (std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    options.configure_locations ().polling_interval = 10ms;
    std::shared_ptr<traced_instance_spot_t> instance;
    options.add_route_mesh ("activation-budget")
      .set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("activation-budget-node"))
      .listen ("tcp://127.0.0.1:0")
      .set_default_request_timeout (80ms)
      .add_instance_spot_factory<traced_instance_spot_t> (
        "traced-player",
        [&] (fw::instance_spot_context_t context) {
            instance = std::make_shared<traced_instance_spot_t> (std::move (context));
            return instance;
        },
        [] (auto &factory) { factory.disable_relocation (); });
    char command[] = "instance-activation-budget";
    char *argv[] = {command};
    int exit_code = -1;
    std::thread host ([&] { exit_code = app.run (1, argv); });
    auto cleanup =
      std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
          store->release.complete (fw::result_t<void>::success ());
          app.stop ();
          host.join ();
      });
    const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    auto provider = app.advanced ().services ().build_provider ();
    auto &client = provider.get_required<fw::route_client_t> ();
    auto send = client.send_to_spot ("budget-player-1", traced_event_t{11})
                  .instance_spot ("traced-player")
                  .in_mesh ("activation-budget")
                  .async ();
    ASSERT_EQ (std::future_status::ready, lookup_entered.wait_for (3s));
    std::this_thread::sleep_for (240ms);
    EXPECT_FALSE (send.await_ready ());
    store->release.complete (fw::result_t<void>::success ());
    // The expired activation deadline reaches the target in ZLIA; send admission succeeds.
    const auto admitted = send.result_for (1s);
    ASSERT_TRUE (admitted.has_value ());
    ASSERT_TRUE (*admitted);
    const auto reply = client.request_to_spot ("budget-player-1", traced_request_t{7})
                         .instance_spot ("traced-player")
                         .in_mesh ("activation-budget")
                         .timeout (3s)
                         .async<traced_reply_t> ()
                         .result_for (4s);
    ASSERT_TRUE (reply.has_value ());
    ASSERT_TRUE (*reply);
    ASSERT_TRUE (instance);
    EXPECT_EQ (0, instance->last_event);
    EXPECT_TRUE (send.result ());
    cleanup.reset ();
    EXPECT_EQ (0, exit_code);
}

TEST (ZLinkFrameworkInstanceSpotActivation, MissingRequestUsesDefaultTimeoutForColdActivation)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::chrono::milliseconds observed_timeout{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
          std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&observed_timeout] (const auto &, const auto &, const auto &, auto, auto, auto,
                           std::chrono::milliseconds timeout, auto) {
          observed_timeout = timeout;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "activation probe completed"));
      });

    const auto result = builder.route_client (serializers)
                          .request_to_spot ("cold-cart", request_t{7})
                          .instance_spot ("shopping-cart")
                          .in_mesh ("commerce")
                          .async<reply_t> ()
                          .result ();

    EXPECT_FALSE (result);
    EXPECT_EQ (std::chrono::seconds (30), observed_timeout);
}

void verify_reserve_loser (bool expire_deadline, bool one_way)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    auto store = std::make_shared<reserve_loser_repository_t> ();
    store->expire_deadline = expire_deadline;
    auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
    auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
    auto relocations = std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
    store->relocations = relocations;
    auto options = host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "reserve-loser",
                              .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"}}};
    std::promise<fw::framework_error_kind_t> diagnostic_promise;
    auto diagnostic_future = diagnostic_promise.get_future ();
    int diagnostics = 0;
    if (one_way)
        fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options.mesh.dispatch, [&] (const fw::message_dispatch_error_event_t &event) {
              ++diagnostics;
              EXPECT_EQ (fw::dispatch_error_action_t::drop, event.action);
              EXPECT_EQ (fw::dispatch_message_kind_t::send, event.message_kind);
              try {
                  std::rethrow_exception (event.exception);
              }
              catch (const fw::framework_exception_t &error) {
                  diagnostic_promise.set_value (error.kind ());
              }
          });
    auto target = std::make_shared<host::public_host_runtime_t> (std::move (options));
    int prepares = 0;
    int handlers = 0;
    target->configure_instance_spot_operations (
      store, relocations, [] { return fw::location_owner_token_t{"target-owner", 1}; },
      host::instance_spot_activation_materializer_t{
        [&] (const auto &, const auto &) {
            ++prepares;
            return true;
        },
        [&] (auto, auto, auto) -> fw::task_t<host::instance_spot_activation_result_t> {
            ++handlers;
            co_return host::instance_spot_activation_result_t{};
        }});
    target->start ();
    const auto deadline = std::chrono::system_clock::now () + (expire_deadline ? 150ms : 3s);
    rt::protocol::instance_spot_activation_header_t request{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "contested", "reserve-loser", "room", "descriptor-1",
       static_cast<std::uint64_t> (
         std::chrono::duration_cast<std::chrono::milliseconds> (deadline.time_since_epoch ())
           .count ())},
      target->status ().lifecycle_generation (),
      target->status ().routing_id ().to_bytes (),
      std::nullopt,
      !one_way,
      {11, 13},
      one_way ? 0u : 17u,
      false};
    rt::protocol::instance_activation_recovery_t command{
      request, std::nullopt,
      rt::protocol::application_payload_t{"probe", "application/json", {1, 2, 3}}};
    int terminals = 0;
    std::uint64_t reply_correlation = 0;
    std::promise<rt::protocol::reply_header_t> reply_promise;
    auto reply_future = reply_promise.get_future ();
    const auto submitted =
      one_way
        ? target
            ->send_instance_spot_activation_remote (target->status ().routing_id (), request,
                                                    command.metadata, command.application_payload)
            .result ()
        : target
            ->activate_instance_spot_remote (
              target->status ().routing_id (), request, command.metadata,
              command.application_payload, 3s,
              [&] (auto terminal, auto header, auto) {
                  EXPECT_EQ (rt::foundation::operation_terminal_t::completed, terminal);
                  ++terminals;
                  reply_promise.set_value (header);
              })
            .result ();
    ASSERT_TRUE (submitted);
    ASSERT_TRUE (submitted.value ());
    const auto pump_deadline = std::chrono::steady_clock::now () + 3s;
    while ((one_way ? diagnostic_future.wait_for (0ms) : reply_future.wait_for (0ms))
             != std::future_status::ready
           && std::chrono::steady_clock::now () < pump_deadline) {
        ASSERT_TRUE (
          target->dispatch_ready ([] (const auto &, const auto &, auto) {}, false).result ());
        std::this_thread::yield ();
    }
    if (one_way) {
        ASSERT_EQ (std::future_status::ready, diagnostic_future.wait_for (0ms));
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, diagnostic_future.get ());
        EXPECT_EQ (1, diagnostics);
        EXPECT_EQ (0, terminals);
    } else {
        ASSERT_EQ (std::future_status::ready, reply_future.wait_for (0ms));
        const auto reply = reply_future.get ();
        reply_correlation = reply.correlation;
        const auto error = rt::messaging::request_failure_mapper_t{}.reply_header_exception (
          reply.terminal_result, reply.failure_code, "Reserve loser");
        EXPECT_EQ (expire_deadline ? fw::framework_error_kind_t::deadline_exceeded
                                   : fw::framework_error_kind_t::unavailable,
                   error.kind ());
        EXPECT_EQ (1, terminals);
        EXPECT_EQ (0, diagnostics);
    }
    EXPECT_EQ (1, store->reads);
    EXPECT_EQ (1, store->reserves);
    EXPECT_EQ (0, prepares);
    EXPECT_EQ (0, handlers);
    ASSERT_TRUE (store->observed);
    EXPECT_EQ (std::chrono::system_clock::time_point (
                 std::chrono::milliseconds (request.target.deadline_unix_ms)),
               store->observed->operation_deadline);
    ASSERT_TRUE (store->captured);
    auto admitted_request = request;
    admitted_request.reply_route_id = reply_correlation;
    EXPECT_EQ (admitted_request, store->captured->activation);
    EXPECT_EQ (command.application_payload.payload_bytes (),
               store->captured->application_payload.payload_bytes ());
    EXPECT_EQ (command.metadata, store->captured->metadata);
    target->close ();
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      ReserveLoserReturnsUnavailableWithoutReadingCurrentOwner)
{
    verify_reserve_loser (false, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ExpiredReserveLoserKeepsTimeoutTerminal)
{
    verify_reserve_loser (true, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, OneWayReserveLoserReportsUnavailableOnce)
{
    verify_reserve_loser (false, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RecoveredRequestWithoutNativeTokenDoesNotSubmitReply)
{
    namespace rt = zlink::framework::runtime;
    rt::mesh::raw_mesh_node_owner_t target (
      {.descriptor = {.mesh_name = "recovered-cold",
                      .node_routing_id = {1},
                      .lifecycle_generation = 1,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"}});
    target.start ();
    rt::mesh::service_mailbox_record_t recovered{
      "source", rt::mesh::service_mailbox_domain_t::infrastructure, {}, {2}, std::nullopt, 17};
    EXPECT_NO_THROW (EXPECT_FALSE (target.reply_instance_spot_activation (recovered, 0, 0)));
    target.close ();
}

TEST (ZLinkFrameworkInstanceSpotActivation, ColdSendUsesServiceSendAdmissionBeforePeerIsAdmitted)
{
    using namespace std::chrono_literals;
    namespace rt = zlink::framework::runtime;
    rt::mesh::raw_mesh_node_owner_t target (
      {.descriptor = {.mesh_name = "cold-admission",
                      .node_routing_id = {1},
                      .lifecycle_generation = 1,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"}});
    rt::mesh::raw_mesh_node_owner_t source (
      {.descriptor = {.mesh_name = "cold-admission",
                      .node_routing_id = {2},
                      .lifecycle_generation = 2,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"}});
    target.start ();
    source.start ();
    ASSERT_TRUE (source.connect_peer (target.endpoint ()));
    ASSERT_FALSE (source.topology ().peer ({1}));
    rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    EXPECT_FALSE (source.send_to_node ({1}, payload).result ().value ());
    rt::protocol::instance_spot_activation_header_t command{
      {{1},
       1,
       "contested",
       "cold-admission",
       "room",
       "descriptor-1",
       static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                     std::chrono::system_clock::now ().time_since_epoch () + 2s)
                                     .count ())},
      2,
      {2},
      std::nullopt,
      false,
      {11, 13},
      0,
      false};
    auto cold = source.send_instance_spot_activation ({1}, command, std::nullopt, payload);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!cold.await_ready () && std::chrono::steady_clock::now () < deadline) {
        ASSERT_TRUE (source.pump_one (rt::mesh::service_liveness_registry_t::clock_t::now (), false)
                       .result ());
        std::this_thread::yield ();
    }
    const auto admitted = cold.result_for (0ms);
    ASSERT_TRUE (admitted);
    ASSERT_TRUE (*admitted);
    EXPECT_FALSE (admitted->value ());
    EXPECT_FALSE (source.topology ().peer ({1}));
    source.close ();
    target.close ();
}

TEST (ZLinkFrameworkInstanceSpotActivation, ColdTypeClassificationUsesServingCapabilities)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    struct case_t
    {
        const char *name;
        int weight;
        int spot_limit;
        int type_limit;
        bool second_type;
        bool fill_first;
        std::optional<std::string> requested_type;
        fw::framework_error_kind_t expected;
    };
    const std::vector<case_t> cases{
      {"absent", 100, 0, 0, false, false, "absent", fw::framework_error_kind_t::not_found},
      {"weight-zero", 0, 0, 0, false, false, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"aggregate-full", 100, 1, 0, false, true, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"per-type-full", 100, 0, 1, false, true, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"multiple-types-after-full", 100, 1, 0, true, true, std::nullopt,
       fw::framework_error_kind_t::invalid_operation}};

    for (const auto &test_case : cases) {
        SCOPED_TRACE (test_case.name);
        auto app = fw::app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_relocation_store (
          std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        options.configure_locations ().polling_interval = 10ms;
        auto mesh = options.add_route_mesh ("cold-type-classification");
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("cold-type-node"))
          .listen ("tcp://127.0.0.1:0")
          .set_placement_weight (test_case.weight);
        if (test_case.spot_limit != 0)
            mesh.set_spot_limit (test_case.spot_limit);
        mesh.add_instance_spot_factory<traced_instance_spot_t> (
          "traced-player",
          [] (fw::instance_spot_context_t context) {
              return std::make_shared<traced_instance_spot_t> (std::move (context));
          },
          [limit = test_case.type_limit] (auto &factory) {
              if (limit)
                  factory.set_stable_type_limit (limit);
              factory.disable_relocation ();
          });
        if (test_case.second_type) {
            mesh.add_instance_spot_factory<traced_instance_spot_t> (
              "other-player",
              [] (fw::instance_spot_context_t context) {
                  return std::make_shared<traced_instance_spot_t> (std::move (context));
              },
              [] (auto &factory) { factory.disable_relocation (); });
        }
        char command[] = "cold-type-classification";
        char *argv[] = {command};
        int exit_code = -1;
        std::thread host ([&] { exit_code = app.run (1, argv); });
        auto cleanup =
          std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
              app.stop ();
              host.join ();
          });
        const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
        while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
            std::this_thread::yield ();
        ASSERT_TRUE (app.is_ready ());
        auto provider = app.advanced ().services ().build_provider ();
        auto &client = provider.get_required<fw::route_client_t> ();
        if (test_case.fill_first) {
            const auto first =
              client.request_to_spot ("filled-spot", traced_request_t{1})
                .instance_spot (test_case.second_type ? "other-player" : "traced-player")
                .in_mesh ("cold-type-classification")
                .timeout (3s)
                .async<traced_reply_t> ()
                .result_for (4s);
            ASSERT_TRUE (first.has_value ());
            ASSERT_TRUE (*first) << (first->error () ? first->error ()->what () : "");
        }
        const auto reply = test_case.requested_type
                             ? client.request_to_spot ("probe-spot", traced_request_t{2})
                                 .instance_spot (*test_case.requested_type)
                                 .in_mesh ("cold-type-classification")
                                 .timeout (3s)
                                 .async<traced_reply_t> ()
                                 .result_for (4s)
                             : client.request_to_spot ("probe-spot", traced_request_t{2})
                                 .instance_spot ()
                                 .in_mesh ("cold-type-classification")
                                 .timeout (3s)
                                 .async<traced_reply_t> ()
                                 .result_for (4s);
        ASSERT_TRUE (reply.has_value ());
        ASSERT_FALSE (*reply);
        EXPECT_EQ (test_case.expected, reply->error_kind ());
        cleanup.reset ();
        EXPECT_EQ (0, exit_code);
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIsNotCachedAndEachOperationAttemptsActivationOnce)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "simulated cold activation rejection");
      },
      [] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, "unused"));
      });

    auto client = builder.route_client (serializers);
    EXPECT_FALSE (
      client.send_to_spot ("missing", event_t{1}).instance_spot ("quest").async ().result ());
    EXPECT_FALSE (
      client.send_to_spot ("missing", event_t{1}).instance_spot ("quest").async ().result ());

    EXPECT_EQ (2, resolver.reads.load ());
    EXPECT_EQ (2, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, ClosingOwnerTerminalInvalidatesBeforeNextColdActivation)
{
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    resolver.addresses.insert_or_assign (
      "player-alice",
      zlink::framework::runtime::spot_address_t{
        "gamequest", zlink::routing_id_t::from ("quest-mission"), "player-alice", 7});
    runtime.bind_spot_address_resolver (resolver);

    std::atomic_int cold_activations{0};
    std::atomic_int ready_failures{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
          std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&serializers, &cold_activations, &ready_failures] (
        const zlink::framework::spot_id_t &spot_id,
        const zlink::framework::detail::spot_activation_intent_t &intent,
        const std::optional<zlink::framework::runtime::spot_address_t> &cached_route, std::string,
        std::type_index, auto, std::chrono::milliseconds,
        auto) -> zlink::framework::task_t<zlink::message_t> {
          EXPECT_EQ ("player-alice", std::string (spot_id));
          EXPECT_EQ (std::optional<std::string> ("gamequest"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("player-quest"), intent.stable_type);
          if (cached_route) {
              ++ready_failures;
              co_return zlink::framework::detail::result_access_t::failure<zlink::message_t> (
                zlink::framework::detail::make_framework_origin_exception (
                  zlink::framework::framework_error_kind_t::shutting_down,
                  "spot serial queue is closed or stopping"));
          }
          ++cold_activations;
          /* Models OnInitialize replaying the durable event stream before the
           * activation-owned first request is dispatched. */
          co_return zlink::framework::result_t<zlink::message_t>::success (
            zlink::framework::detail::encoded_payload_to_raw (
              serializers.get<reply_t> ().serialize (reply_t{3})));
      });

    messaging::envelope_codec_t envelopes;
    zlink::framework::detail::channel_reply_writer_t replies;
    std::atomic_int direct_requests{0};
    runtime.bind_spot_mesh_transport (
      "gamequest",
      [] (const auto &, const auto &, std::uint64_t,
          auto) -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure, "unused direct send");
      },
      [&direct_requests, &envelopes, &replies] (const zlink::routing_id_t &, const std::string &,
                                                std::uint64_t, messaging::message_parts_t parts,
                                                std::chrono::milliseconds)
        -> zlink::framework::task_t<zlink::framework::result_t<messaging::message_parts_t>> {
          ++direct_requests;
          const auto request_header = envelopes.decode_header (parts);
          if (!request_header) {
              co_return zlink::framework::result_t<messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::protocol_error,
                "request header decode failed");
          }
          const auto error_header =
            replies.create_error_header ("gamequest", request_header.value (),
                                         zlink::framework::detail::make_framework_origin_exception (
                                           zlink::framework::framework_error_kind_t::shutting_down,
                                           "spot serial queue is closed or stopping"));
          co_return zlink::framework::result_t<messaging::message_parts_t>::success (
            replies.reply_raw_envelope (error_header, zlink::message_t::from ("")));
      });

    auto client = builder.route_client (serializers);
    const auto stale = client.request_to_spot ("player-alice", request_t{1})
                         .instance_spot ("player-quest")
                         .in_mesh ("gamequest")
                         .async<reply_t> ()
                         .result ();
    ASSERT_FALSE (stale);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::shutting_down, stale.error_kind ());
    EXPECT_EQ (0, direct_requests.load ());
    EXPECT_EQ (1, ready_failures.load ());
    EXPECT_EQ (0, cold_activations.load ());
    EXPECT_FALSE (resolver.addresses.contains ("player-alice"));

    const auto rehydrated = client.request_to_spot ("player-alice", request_t{2})
                              .instance_spot ("player-quest")
                              .in_mesh ("gamequest")
                              .async<reply_t> ()
                              .result ();
    ASSERT_TRUE (rehydrated);
    EXPECT_EQ (3, rehydrated.value ().value);
    EXPECT_EQ (0, direct_requests.load ());
    EXPECT_EQ (1, ready_failures.load ());
    EXPECT_EQ (1, cold_activations.load ());
    EXPECT_EQ (2, resolver.reads.load ());
}

// Failover policy §4.4: an Instance intent request whose cached Ready route the
// owner fence refused before admission reads the authority once. Missing continues
// as cold activation; a present authority or another terminal ends the operation.
TEST (ZLinkFrameworkInstanceSpotActivation, CachedRouteFenceRefusalReadsAuthorityOnce)
{
    namespace messaging = zlink::framework::runtime::messaging;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::spot_address_t;

    class authority_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
    {
      public:
        zlink::framework::task_t<std::optional<spot_address_t>>
        resolve_spot_address (std::string, std::string) override
        {
            if (cached)
                co_return cached;
            ++authority_reads;
            co_return authority;
        }

        void invalidate_spot_address (std::string_view) override { cached.reset (); }

        void invalidate_all_routes_after_store_recovery () override { cached.reset (); }

        std::optional<spot_address_t> cached;
        std::optional<spot_address_t> authority;
        int authority_reads = 0;
    };

    const messaging::request_failure_mapper_t mapper;
    // The owner's Ready owner-fence refusal, and the Unavailable terminal of a message
    // the owner accepted (a relocating host ending the Instance intent messages it kept).
    const auto refusal = *mapper.target_failure_reply (
      framework_error_kind_t::unavailable,
      static_cast<std::uint32_t> (
        zlink::framework::runtime::protocol::framework_error_code::spotMoving));
    const auto accepted = *mapper.target_failure_reply (framework_error_kind_t::unavailable);
    struct case_t
    {
        messaging::request_wire_failure_t owner_failure;
        bool authority_present;
        bool replied;
        std::vector<bool> cached_routes;
        int authority_reads;
    };
    for (const auto &scenario :
         {case_t{refusal, false, true, {true, false}, 1}, case_t{refusal, true, false, {true}, 1},
          case_t{accepted, false, false, {true}, 0}}) {
        zlink::framework::serializer_registry_t serializers;
        zlink::framework::zlink_builder_t builder =
          zlink::framework::test::runtime_failure_builder ();
        auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        const spot_address_t route{"gamequest", zlink::routing_id_t::from ("quest-mission"),
                                   "player-alice", 7};
        authority_resolver_t resolver;
        resolver.cached = route;
        if (scenario.authority_present)
            resolver.authority = route;
        runtime.bind_spot_address_resolver (resolver);
        std::vector<bool> cached_routes;
        runtime.bind_instance_spot_activator (
          [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
              std::chrono::system_clock::time_point)
            -> zlink::framework::task_t<zlink::framework::result_t<void>> {
              co_return zlink::framework::result_t<void>::failure (
                framework_error_kind_t::internal_failure, "unused one-way activation");
          },
          [&] (const auto &, const auto &, const std::optional<spot_address_t> &cached_route,
               std::string, std::type_index, auto, std::chrono::milliseconds,
               auto) -> zlink::framework::task_t<zlink::message_t> {
              cached_routes.push_back (cached_route.has_value ());
              if (cached_route)
                  co_return zlink::framework::detail::result_access_t::failure<zlink::message_t> (
                    mapper.reply_header_exception (scenario.owner_failure.terminal_result,
                                                   scenario.owner_failure.failure_code,
                                                   "Instance Spot request"));
              co_return zlink::framework::result_t<zlink::message_t>::success (
                zlink::framework::detail::encoded_payload_to_raw (
                  serializers.get<reply_t> ().serialize (reply_t{9})));
          });

        const auto reply = builder.route_client (serializers)
                             .request_to_spot ("player-alice", request_t{1})
                             .instance_spot ("player-quest")
                             .async<reply_t> ()
                             .result ();
        EXPECT_EQ (scenario.replied, static_cast<bool> (reply));
        if (reply) {
            EXPECT_EQ (9, reply.value ().value);
        } else {
            EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
            ASSERT_TRUE (reply.error ());
            EXPECT_EQ (scenario.owner_failure.failure_code,
                       zlink::framework::detail::failure_code (*reply.error ()));
        }
        EXPECT_EQ (scenario.cached_routes, cached_routes);
        EXPECT_EQ (scenario.authority_reads, resolver.authority_reads);
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, RetiredOwnerRequestCompletesWithUnavailableTerminal)
{
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("retired-owner");
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder, "retired-owner");
    ASSERT_TRUE (runtime);
    zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
      .bind_serializers (serializers);
    runtime->set_route_client (builder.route_client (serializers));

    zlink::framework::service_collection_t services;
    services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();

    const auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "retired-owner",
                              .node_routing_id =
                                zlink::routing_id_t::from ("retired-owner-reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"}}});
    std::vector<zlink::message_t> reply_parts;
    host::receive_record_t record{
      .kind = host::record_kind_t::spot_request,
      .domain = host::ready_domain_t::application,
      .spot_route =
        zlink::framework::runtime::protocol::spot_route_fence_t{"retired-spot", 1, {}, 1, 1, 1}};
    record.reply_token.host = reply_host;
    record.reply_token.local_reply = [&reply_parts] (const std::vector<zlink::message_t> &parts) {
        reply_parts = parts;
        return true;
    };
    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::spot,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = "retired-spot"};
    messaging::envelope_codec_t codec;
    auto encoded = codec.encode_parts (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "retired-owner",
                                   .message_name = traced_request_t::packet_name,
                                   .correlation_id = "retired-request"},
      traced_request_t{7}, serializers);
    auto request_parts = std::move (encoded).take_items ();

    EXPECT_TRUE (
      runtime->dispatch_mesh_record (owner, record, request_parts, provider, serializers));
    ASSERT_EQ (2u, reply_parts.size ());
    const auto reply_header = codec.decode_header (messaging::message_parts_t (reply_parts));
    ASSERT_TRUE (reply_header);
    EXPECT_EQ (messaging::message_kind_t::error, reply_header.value ().kind);
    EXPECT_EQ ("unavailable", reply_header.value ().error_code.value_or (""));
    EXPECT_EQ ("Spot route owner is no longer registered",
               reply_header.value ().error_message.value_or (""));
    /* Framework-generated route errors carry the zlink.origin=framework
     * marker so callers can tell them apart from application errors. */
    const auto &reply_metadata = reply_header.value ().metadata;
    const auto origin = reply_metadata.find ("zlink.origin");
    ASSERT_NE (origin, reply_metadata.end ());
    EXPECT_EQ ("framework", origin->second);
    EXPECT_TRUE (messaging::has_framework_origin (reply_metadata));
}

TEST (ZLinkFrameworkInstanceSpotActivation, FrameworkOriginMarkerIsAttachedOnlyToFrameworkErrors)
{
    namespace messaging = zlink::framework::runtime::messaging;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::framework_exception_t;

    messaging::envelope_header_t request;
    request.kind = messaging::message_kind_t::request;
    request.channel_name = "origin-mesh";
    request.message_name = "origin.request";
    request.correlation_id = "origin-request";

    zlink::framework::detail::channel_reply_writer_t replies;

    /* Framework-generated failure: marker attached. */
    const auto framework_reply = replies.create_error_header (
      "origin-mesh", request,
      zlink::framework::detail::make_framework_origin_exception (framework_error_kind_t::not_found,
                                                                 "spot handler is not registered"));
    EXPECT_TRUE (messaging::has_framework_origin (framework_reply.metadata));

    /* Application handler failure: no marker. */
    const auto application_reply = replies.create_error_header (
      "origin-mesh", request,
      framework_exception_t (framework_error_kind_t::not_found, "application says not found"));
    EXPECT_FALSE (messaging::has_framework_origin (application_reply.metadata));
    EXPECT_EQ (application_reply.metadata.find ("zlink.origin"), application_reply.metadata.end ());

    /* The marker survives the envelope wire round trip. */
    messaging::envelope_codec_t codec;
    auto parts = codec.encode_raw_body_parts (framework_reply, zlink::message_t::from (""));
    const auto decoded = codec.decode_header (parts);
    ASSERT_TRUE (decoded);
    EXPECT_TRUE (messaging::has_framework_origin (decoded.value ().metadata));

    /* Caller-side classification: unmarked remote errors are application
     * origin and must not be read as a stale-route signal. */
    using zlink::framework::detail::error_origin_t;
    const auto marked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (decoded.value ().metadata) ? error_origin_t::framework
                                                                  : error_origin_t::application);
    EXPECT_EQ (error_origin_t::framework, zlink::framework::detail::error_origin (marked));
    const auto unmarked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (application_reply.metadata) ? error_origin_t::framework
                                                                   : error_origin_t::application);
    EXPECT_EQ (error_origin_t::application, zlink::framework::detail::error_origin (unmarked));
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      ColdActivationDispatchEmitsReceivedAndTerminalFlowEvents)
{
    zlink::framework::serializer_registry_t serializers;

    std::mutex events_mutex;
    std::condition_variable events_changed;
    std::vector<zlink::framework::message_flow_event_t> events;
    zlink::framework::dispatch_options_t dispatch;
    dispatch.message_flow (zlink::framework::message_flow_log_mode_t::normal);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch, [&] (const zlink::framework::message_flow_event_t &event) {
          {
              const std::lock_guard lock (events_mutex);
              events.push_back (event);
          }
          events_changed.notify_all ();
      });

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    zlink::framework::detail::apply_dispatch_options (builder, dispatch);
    auto mesh = builder.add_route_mesh ("instance-trace");
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [] (zlink::framework::instance_spot_context_t context) {
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder, "instance-trace");
    ASSERT_TRUE (runtime);
    auto channel_runtime =
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    channel_runtime.bind_serializers (serializers);

    const auto created = runtime->get_or_create_spot (
      "traced-player", zlink::framework::spot_id_t ("traced-player-1"));
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const std::string activation_flow_id = "019fc5b9-9df3-786b-bb69-d55358f6d48b";
    const auto event_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_event_t> ().serialize (traced_event_t{7}));
    const auto event_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_event_t::packet_name,
          serializers.get<traced_event_t> ().content_type (), event_payload.to_bytes (), {}, false,
          "operation-send", provider, serializers, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (event_result);

    const auto request_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_request_t> ().serialize (traced_request_t{9}));
    const auto request_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_request_t::packet_name,
          serializers.get<traced_request_t> ().content_type (), request_payload.to_bytes (), {},
          true, "operation-request", provider, serializers, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (request_result);
    const auto decoded_reply = serializers.get<traced_reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (request_result.value ()));
    EXPECT_EQ (10, decoded_reply.value);

    {
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (lock, std::chrono::seconds (2),
                                              [&] { return events.size () >= 4; }));
    }

    const auto has_event_transition = [&] (std::string_view packet,
                                           zlink::framework::message_flow_outcome_t outcome,
                                           std::string_view correlation) {
        const std::lock_guard lock (events_mutex);
        return std::any_of (events.begin (), events.end (), [&] (const auto &event) {
            return event.packet_name && *event.packet_name == packet && event.outcome == outcome
                   && event.surface == zlink::framework::dispatch_error_surface_t::spot_route
                   && event.spot_id && *event.spot_id == "traced-player-1" && event.correlation_id
                   && *event.correlation_id == correlation && event.flow_id
                   && *event.flow_id == activation_flow_id && event.flow_origin
                   && *event.flow_origin == zlink::framework::flow_origin_t::application;
        });
    };
    EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::received,
                                       "operation-send"));
    EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::dispatched,
                                       "operation-send"));
    EXPECT_TRUE (has_event_transition (traced_request_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::received,
                                       "operation-request"));
    EXPECT_TRUE (has_event_transition (traced_request_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::replied,
                                       "operation-request"));
}

} // namespace

static void verify_ended_instance_intent (bool provider, bool disabled)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_repository_t> store =
      provider ? std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::provider_location_repository_t> (*opaque))
               : std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::in_memory_location_repository_t> ());
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("ended-instance-owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t old;
    old.mesh_name = "ended-instance";
    old.rid = zlink::routing_id_t::from ("old-instance-target");
    old.lifecycle_generation = 1;
    old.descriptor_revision = 1;
    old.endpoint = "tcp://127.0.0.1:7101";
    old.security_identity = "old-instance-target";
    old.owner_id = owner.owner_id;
    old.lease_generation = owner.lease_generation;
    old.object_role = fw::object_role_t::server;
    old.state = fw::framework_runtime_state_t::serving;
    // The old owner's advertised policy must not authorize the new target.
    old.object_capabilities.push_back (
      {fw::placement_object_kind_t::instance_spot, "traced-player",
       disabled ? fw::maintenance_policy_kind_t::recreate : fw::maintenance_policy_kind_t::disabled,
       false, 1});
    old.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "traced-player", {0, 0, 0}});
    ASSERT_EQ (fw::location_write_status_t::stored,
               store->update_mesh_node (old, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    fw::object_reserve_request_t initial;
    initial.key = {fw::placement_object_kind_t::instance_spot, "ended-room"};
    initial.intent.stable_type = "traced-player";
    initial.target = {old.mesh_name, fw::node_rid_t::from_string (old.rid.to_string ()), 1, owner};
    initial.capacity_bundle = {0, 1,
                               fw::spot_type_capacity_delta_t{
                                 fw::placement_object_kind_t::instance_spot, "traced-player", 1}};
    const auto initial_result = store->reserve (initial).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (initial_result))
      << initial_result.index ();
    const auto reserved = std::get<fw::object_reserved_t> (initial_result);
    rt::instance_spot_authority_payload_t ready{
      .stable_type = "traced-player",
      .spot_id = "ended-room",
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = old.mesh_name,
      .node_rid = initial.target.node_rid,
      .node_generation = 1};
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      store
        ->commit ({initial.key, reserved.fence, rt::encode_instance_spot_authority_payload (ready)})
        .result ()
        .value ()));
    const auto before = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    ASSERT_TRUE (std::holds_alternative<fw::owner_lease_released_t> (
      store->release_owner_lease (owner).result ().value ()));
    auto app = fw::app_t::create ();
    auto &options = app.add_zlink_framework ();
    options.add_location_store (opaque);
    options.add_relocation_store (std::make_shared<rt::in_memory_relocation_store_t> ());
    app.advanced ().services ().add_factory<fw::location_repository_t> (
      [store] (fw::service_provider_t &) { return store; }, fw::service_lifetime_t::singleton);
    options.configure_locations ().polling_interval = 10ms;
    auto mesh = options.add_route_mesh ("ended-instance");
    mesh.set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("new-instance-target"))
      .listen ("tcp://127.0.0.1:0");
    std::atomic_int factories{0};
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [&] (fw::instance_spot_context_t context) {
          ++factories;
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [disabled] (auto &factory) {
          if (disabled)
              factory.disable_relocation ();
          else
              factory.recreate_on_relocation ();
      });
    std::thread host ([&] { EXPECT_EQ (0, app.run (0, nullptr)); });
    auto cleanup =
      std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
          app.stop ();
          host.join ();
      });
    const auto until = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < until)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    auto services = app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::route_client_t> ();
    (void) services.get_required<fw::spot_manager_t> ().find ("ended-room").result ();
    const auto ordinary = client.request_to_spot ("ended-room", traced_request_t{1})
                            .timeout (3s)
                            .async<traced_reply_t> ()
                            .result ();
    EXPECT_FALSE (ordinary);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, ordinary.error_kind ());
    const auto retained = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    EXPECT_EQ (before.store_version, retained.store_version);
    EXPECT_EQ (before.payload, retained.payload);
    EXPECT_EQ (0, factories);
    const auto wrong = client.request_to_spot ("ended-room", traced_request_t{2})
                         .instance_spot ("other-type")
                         .timeout (3s)
                         .async<traced_reply_t> ()
                         .result ();
    EXPECT_FALSE (wrong);
    EXPECT_EQ (fw::framework_error_kind_t::type_mismatch, wrong.error_kind ());
    const auto result = client.request_to_spot ("ended-room", traced_request_t{3})
                          .instance_spot ()
                          .in_mesh ("ignored-on-existing-id")
                          .timeout (3s)
                          .async<traced_reply_t> ()
                          .result ();
    const auto after = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    if (disabled) {
        ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "");
        EXPECT_EQ (4, result.value ().value);
        EXPECT_GT (after.object_generation, before.object_generation);
        EXPECT_EQ (1, factories);
        const auto joined = client.request_to_spot ("ended-room", traced_request_t{4})
                              .instance_spot ()
                              .timeout (3s)
                              .async<traced_reply_t> ()
                              .result ();
        ASSERT_TRUE (joined);
        EXPECT_EQ (5, joined.value ().value);
        EXPECT_EQ (1, factories);
        EXPECT_EQ (
          after.object_generation,
          std::get<fw::authority_snapshot_t> (
            store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ())
            .object_generation);
    } else {
        EXPECT_FALSE (result);
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, result.error_kind ());
        EXPECT_EQ (before.store_version, after.store_version);
        EXPECT_EQ (before.payload, after.payload);
        EXPECT_EQ (0, factories);
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, EndedReadyProviderUsesTargetPolicy)
{
    verify_ended_instance_intent (true, true);
    verify_ended_instance_intent (true, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, EndedReadyInMemoryUsesTargetPolicy)
{
    verify_ended_instance_intent (false, true);
    verify_ended_instance_intent (false, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, StoredActivationMismatchPreservesAuthority)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    for (bool provider : {false, true}) {
        for (bool active : {false, true}) {
            for (unsigned input = 0; input != 8; ++input) {
                SCOPED_TRACE (active);
                SCOPED_TRACE (provider);
                SCOPED_TRACE (input);
                auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
                std::shared_ptr<fw::location_repository_t> store =
                  provider ? std::static_pointer_cast<fw::location_repository_t> (
                               std::make_shared<rt::provider_location_repository_t> (*opaque))
                           : std::static_pointer_cast<fw::location_repository_t> (
                               std::make_shared<rt::in_memory_location_repository_t> ());
                const auto owner =
                  std::get<fw::owner_lease_claimed_t> (
                    store->claim_owner_lease ("stored-owner", 60s).result ().value ())
                    .token;
                fw::mesh_node_descriptor_t descriptor;
                descriptor.mesh_name = "stored-activation";
                descriptor.rid = zlink::routing_id_t::from ("stored-target");
                descriptor.lifecycle_generation = 1;
                descriptor.descriptor_revision = 1;
                descriptor.endpoint = "tcp://127.0.0.1:7101";
                descriptor.security_identity = "stored-target";
                descriptor.owner_id = owner.owner_id;
                descriptor.lease_generation = owner.lease_generation;
                descriptor.object_role = fw::object_role_t::server;
                descriptor.state = fw::framework_runtime_state_t::serving;
                descriptor.object_capabilities.push_back (
                  {fw::placement_object_kind_t::instance_spot, "room",
                   fw::maintenance_policy_kind_t::disabled, false, 1});
                descriptor.capacity.spot_types.push_back (
                  {fw::placement_object_kind_t::instance_spot, "room", {0, 0, 0}});
                ASSERT_EQ (
                  fw::location_write_status_t::stored,
                  store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                    .result ()
                    .value ()
                    .status);
                auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
                auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
                auto relocations =
                  std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
                auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
                  .mesh = {.descriptor = {.mesh_name = descriptor.mesh_name,
                                          .node_routing_id = descriptor.rid.to_bytes (),
                                          .lifecycle_generation = 1,
                                          .descriptor_revision = 1,
                                          .advertised_endpoint = "tcp://127.0.0.1:0"}}});
                int factories = 0;
                target->configure_instance_spot_operations (
                  store, relocations, [owner] { return owner; },
                  host::instance_spot_activation_materializer_t{
                    [&] (const auto &, const auto &) {
                        ++factories;
                        return true;
                    },
                    [] (auto, auto, auto) -> fw::task_t<host::instance_spot_activation_result_t> {
                        co_return host::instance_spot_activation_result_t{};
                    }});
                target->start ();
                auto cleanup =
                  std::unique_ptr<host::public_host_runtime_t,
                                  std::function<void (host::public_host_runtime_t *)>> (
                    target.get (), [&] (auto *) { target->close (); });
                const auto deadline = std::chrono::system_clock::now () + 3s;
                rt::protocol::instance_spot_activation_header_t original{
                  {descriptor.rid.to_bytes (), 1, "stored-room", descriptor.mesh_name, "room", "1",
                   static_cast<std::uint64_t> (
                     std::chrono::duration_cast<std::chrono::milliseconds> (
                       deadline.time_since_epoch ())
                       .count ())},
                  1,
                  descriptor.rid.to_bytes (),
                  std::nullopt,
                  true,
                  {11, 13},
                  17,
                  true};
                rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
                const auto metadata =
                  fw::detail::mesh_metadata_codec_t::encode ({{"key", "value"}});
                const auto encoded =
                  rt::protocol::encode_instance_activation_recovery ({original, metadata, payload});
                const auto receipt =
                  relocations->put (encoded, 24h, std::chrono::steady_clock::now () + 3s);
                fw::object_reserve_request_t reserve;
                reserve.key = {fw::placement_object_kind_t::instance_spot, "stored-room"};
                reserve.intent.stable_type = "room";
                reserve.intent.request_content_reference = receipt.reference;
                std::vector<std::byte> public_bytes;
                for (auto value : encoded)
                    public_bytes.push_back (static_cast<std::byte> (value));
                reserve.intent.request_sha256 = rt::sha256 (public_bytes);
                reserve.intent.request_encoded_size = public_bytes.size ();
                reserve.target = {descriptor.mesh_name,
                                  fw::node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                                  owner};
                reserve.capacity_bundle = {
                  0, 1,
                  fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::instance_spot, "room",
                                                 1}};
                const auto reserved_result = store->reserve (reserve).result ().value ();
                ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (reserved_result));
                if (active) {
                    rt::instance_spot_authority_payload_t ready{
                      .stable_type = "room",
                      .spot_id = "stored-room",
                      .owner_id = owner.owner_id,
                      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
                      .mesh_name = descriptor.mesh_name,
                      .node_rid = reserve.target.node_rid,
                      .node_generation = 1,
                      .activation_recovery = rt::activation_recovery_pointer_t{
                        receipt.reference, reserve.intent.request_sha256,
                        static_cast<std::uint32_t> (reserve.intent.request_encoded_size), 1, 0}};
                    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
                      store
                        ->commit ({reserve.key,
                                   std::get<fw::object_reserved_t> (reserved_result).fence,
                                   rt::encode_instance_spot_authority_payload (ready)})
                        .result ()
                        .value ()));
                }
                const auto capacity_before = store->list_mesh_nodes (descriptor.mesh_name)
                                               .result ()
                                               .value ()
                                               .items.front ()
                                               .capacity;
                const auto before = std::get<fw::authority_snapshot_t> (
                  store->read_authority (rt::spot_authority_key ("stored-room"))
                    .result ()
                    .value ());
                auto wrong = original;
                auto changed_metadata = std::optional<std::vector<std::uint8_t>> (metadata);
                switch (input) {
                    case 0:
                        wrong.target.mesh_name = "other-mesh";
                        break;
                    case 1:
                        wrong.target.stable_type = "other-type";
                        break;
                    case 2:
                        wrong.target.descriptor_version = "2";
                        break;
                    case 3:
                        ++wrong.target.deadline_unix_ms;
                        break;
                    case 4:
                        ++wrong.operation.low;
                        ++wrong.target.deadline_unix_ms;
                        break;
                    case 5:
                        wrong.has_metadata = false;
                        changed_metadata.reset ();
                        break;
                    case 6:
                        changed_metadata =
                          fw::detail::mesh_metadata_codec_t::encode ({{"key", "other"}});
                        break;
                    case 7:
                        break; // Matching retransmission uses the current authority.
                }
                std::promise<rt::protocol::reply_header_t> reply;
                auto result = reply.get_future ();
                ASSERT_TRUE (target
                               ->activate_instance_spot_remote (
                                 descriptor.rid, wrong, changed_metadata, payload, 3s,
                                 [&] (auto, auto header, auto) { reply.set_value (header); })
                               .result ()
                               .value ());
                const auto until = std::chrono::steady_clock::now () + 3s;
                while (result.wait_for (0ms) != std::future_status::ready
                       && std::chrono::steady_clock::now () < until) {
                    ASSERT_TRUE (
                      target->dispatch_ready ([] (const auto &, const auto &, auto) {}, false)
                        .result ());
                    std::this_thread::yield ();
                }
                ASSERT_EQ (std::future_status::ready, result.wait_for (0ms));
                const auto terminal = result.get ();
                const auto expected =
                  input != 7 && input != 4 ? rt::protocol::request_terminal_result::protocolError
                  : active                 ? rt::protocol::request_terminal_result::ok
                                           : rt::protocol::request_terminal_result::internalError;
                EXPECT_EQ (static_cast<std::uint32_t> (expected), terminal.terminal_result);
                const auto after = std::get<fw::authority_snapshot_t> (
                  store->read_authority (rt::spot_authority_key ("stored-room"))
                    .result ()
                    .value ());
                EXPECT_EQ (before.store_version, after.store_version);
                EXPECT_EQ (before.payload, after.payload);
                EXPECT_EQ (before.object_generation, after.object_generation);
                if (!active) {
                    ASSERT_TRUE (after.pending_creation);
                    EXPECT_EQ (before.pending_creation->request_content_reference,
                               after.pending_creation->request_content_reference);
                }
                const auto capacity_after = store->list_mesh_nodes (descriptor.mesh_name)
                                              .result ()
                                              .value ()
                                              .items.front ()
                                              .capacity;
                EXPECT_EQ (capacity_before.spots.active, capacity_after.spots.active);
                EXPECT_EQ (capacity_before.spots.reserved, capacity_after.spots.reserved);
                EXPECT_EQ (capacity_before.spot_types.front ().usage.active,
                           capacity_after.spot_types.front ().usage.active);
                EXPECT_EQ (capacity_before.spot_types.front ().usage.reserved,
                           capacity_after.spot_types.front ().usage.reserved);
                EXPECT_EQ (encoded, *relocations->get (receipt.reference));
                EXPECT_EQ ((input == 7 || input == 4) && active ? 1 : 0, factories);
            }
        }
    }
}

TEST (CppFrameworkInstanceSpotActivation, StartupReleasesPreviousLifecycleBeforeRecoveryRoot)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    auto provider = std::make_shared<rt::in_memory_location_store_t> ();
    auto store = std::make_shared<rt::provider_location_repository_t> (*provider);
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("startup-owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "startup-mesh";
    descriptor.rid = zlink::routing_id_t::from ("startup-target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7100";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::instance_spot, "room",
                                               fw::maintenance_policy_kind_t::disabled, false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "room", {0, 0, 1}});
    ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    class roots_t final : public rt::stateful::relocation_store_port_t
    {
      public:
        std::function<void ()> before_remove;
        bool exists = true;
        rt::stateful::relocation_stored_t put (const std::vector<std::uint8_t> &,
                                               std::chrono::hours,
                                               std::chrono::steady_clock::time_point) override
        {
            return {"startup-root", 0};
        }
        std::optional<std::vector<std::uint8_t>> get (const std::string &) override
        {
            ADD_FAILURE () << "Old lifecycle must not replay";
            return std::nullopt;
        }
        void remove (const std::string &reference) override
        {
            EXPECT_EQ (reference, "startup-root");
            before_remove ();
            EXPECT_TRUE (exists);
            exists = false;
        }
    };
    auto roots = std::make_shared<roots_t> ();
    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::instance_spot, "startup-spot"};
    request.intent = {"room", "startup-root", {}, 4};
    request.target = {descriptor.mesh_name, fw::node_rid_t::from_string ("startup-target"), 1,
                      owner};
    const std::string marker = "zlink:instance-spot:creating:v1";
    for (const auto ch : marker)
        request.creating_payload.push_back (static_cast<std::byte> (ch));
    request.capacity_bundle.spot_slots = 1;
    request.capacity_bundle.spot_type =
      fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::instance_spot, "room", 1};
    const auto reserved = store->reserve (request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (reserved));
    const auto key = rt::spot_authority_key ("startup-spot");
    roots->before_remove = [&] {
        EXPECT_TRUE (std::holds_alternative<fw::authority_missing_t> (
          store->read_authority (key).result ().value ()));
        const auto capacity = std::get<fw::store_found_t> (
          provider->read ({"zlink:v11:capacity:startup-mesh:startup-target"}).result ().value ());
        const auto counts =
          nlohmann::json::parse (capacity.value.bytes.begin (), capacity.value.bytes.end ());
        EXPECT_EQ (counts.at ("pending").at ("spots"), 0);
    };
    store->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner).result ().value ();
    descriptor.lifecycle_generation = 2;
    ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{.mesh = {.descriptor = {.mesh_name = "startup-mesh",
                                                   .node_routing_id = descriptor.rid.to_bytes (),
                                                   .lifecycle_generation = 2,
                                                   .descriptor_revision = 1,
                                                   .advertised_endpoint = "tcp://127.0.0.1:0"}}});
    target->configure_instance_spot_operations (
      store, roots, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [] (const auto &, const auto &) {
            ADD_FAILURE () << "Old lifecycle must not prepare";
            return false;
        },
        [] (auto, auto, auto) -> fw::task_t<host::instance_spot_activation_result_t> {
            ADD_FAILURE () << "Old lifecycle must not activate";
            co_return host::instance_spot_activation_result_t{};
        }});
    ASSERT_TRUE (roots->exists);
    target->start ();
    target->recover_instance_spot_activations ();
    EXPECT_FALSE (roots->exists);
}
