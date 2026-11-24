/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/actors/actor_client.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_counting_location_repository.hpp"
#include "../support/m6b_task.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <zlink/socket/api.h>
#endif

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;

#if !defined(_WIN32)
namespace
{
std::atomic_bool count_request_submissions{false};
std::atomic_int request_submissions{0};
}
extern "C" zlink_submit_result_t zlink_request (void *socket,
                                                const zlink_routing_id_t *target,
                                                zlink_msg_t *parts,
                                                size_t part_count,
                                                zlink_send_flags_t flags,
                                                uint32_t timeout_ms,
                                                void *context,
                                                zlink_completion_id_t *completion)
{
    using request_fn_t = decltype (&zlink_request);
    static const auto request = reinterpret_cast<request_fn_t> (dlsym (RTLD_NEXT, "zlink_request"));
    if (!request)
        std::abort ();
    if (count_request_submissions.load (std::memory_order_acquire))
        request_submissions.fetch_add (1, std::memory_order_relaxed);
    return request (socket, target, parts, part_count, flags, timeout_ms, context, completion);
}
#endif

namespace zlink::framework::test::m6b
{
class actor_send_spot_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        co_return spot_id == address.spot_id ? std::make_optional (address) : std::nullopt;
    }

    void invalidate_spot_address (std::string_view) override {}

    void invalidate_all_routes_after_store_recovery () override {}

    zlink::framework::runtime::spot_address_t address;
};
void verify_actor_client_authority_generation (bool invalidated)
{
    using namespace zlink::framework;
    auto store = std::make_shared<counting_location_repository_t> ();
    const auto owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("actor-generation-owner", 30s).result ().value ())
        .token;
    auto context = std::make_shared<zlink::context_t> ();
    const auto make_runtime = [&] (std::string rid) {
        auto state =
          zlink::framework::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (
            "actor-send-route-mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (std::move (rid));
        state->core_context = context;
        return std::make_shared<detail::mesh_node_runtime_t> (state);
    };
    auto source = make_runtime ("actor-generation-source");
    auto target = make_runtime ("actor-generation-target");
    source->start ().result ().value ();
    target->start ().result ().value ();
    const auto source_status = source->status ();
    const auto target_status = target->status ();
    source->connect_peer (target_status.routing_id (), target_status.local_endpoint (),
                          target_status.lifecycle_generation ());
    target->connect_peer (source_status.routing_id (), source_status.local_endpoint (),
                          source_status.lifecycle_generation ());
    const auto connect_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!source->has_admitted_peer (target_status.routing_id (),
                                        target_status.lifecycle_generation ())
            || !target->has_admitted_peer (source_status.routing_id (),
                                           source_status.lifecycle_generation ()))
           && std::chrono::steady_clock::now () < connect_deadline) {
        (void) await_task (
          source->native_node ().transport ().pump_one (std::chrono::steady_clock::now ()));
        (void) await_task (
          target->native_node ().transport ().pump_one (std::chrono::steady_clock::now ()));
    }
    assert (source->has_admitted_peer (target_status.routing_id (),
                                       target_status.lifecycle_generation ()));
    assert (target->has_admitted_peer (source_status.routing_id (),
                                       source_status.lifecycle_generation ()));
    const auto generation =
      target_status.lifecycle_generation () == std::numeric_limits<std::uint64_t>::max ()
        ? 1
        : std::numeric_limits<std::uint64_t>::max ();
    source->expect_peer (target_status.routing_id (), target_status.local_endpoint (), generation,
                         "default");
    const auto publish_target = [&] (const detail::mesh_node_runtime_t &runtime,
                                     const location_owner_token_t &owner) {
        const auto status = runtime.status ();
        mesh_node_descriptor_t node{
          .mesh_name = "actor-send-route-mesh",
          .rid = status.routing_id (),
          .lifecycle_generation = generation,
          .descriptor_revision = 1,
          .endpoint = status.local_endpoint (),
          .application_version = 1,
          .object_capabilities = {{.object_kind = placement_object_kind_t::actor,
                                   .stable_type = "route-probe"}},
          .object_role = object_role_t::server,
          .capacity = {.actors = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "actor-send-route-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store->update_mesh_node (std::move (node), location_write_intent_t::new_claim)
                  .result ()
                  .value ()
                  .status
                == location_write_status_t::stored);
    };

    publish_target (*target, owner);
    const object_creation_key_t key{placement_object_kind_t::actor, "actor-generation-probe"};
    const object_creation_target_t placement{
      "actor-send-route-mesh", node_rid_t::from_string (target_status.routing_id ().to_string ()),
      generation, owner};
    const auto reserved =
      std::get<object_reserved_t> (store
                                     ->reserve ({.key = key,
                                                 .intent = {.stable_type = "route-probe"},
                                                 .target = placement,
                                                 .capacity_bundle = {.actor_slots = 1}})
                                     .result ()
                                     .value ());
    const auto payload =
      runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
        .stable_type = "route-probe",
        .actor_id = key.global_id,
        .current_spot_id = target->native_node ().entry_spot ().spot_id (),
        .current_spot_generation = target_status.lifecycle_generation (),
        .current_spot_kind = runtime::actor_authority_spot_kind_t::entry,
        .owner_id = owner.owner_id,
        .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
        .mesh_name = placement.mesh_name,
        .node_rid = placement.node_rid,
        .node_generation = generation});
    assert (std::holds_alternative<object_committed_t> (
      store->commit ({key, reserved.fence, payload}).result ().value ()));
    location_options_t options;
    options.owner_lease_fencing_margin = 1s;
    runtime::live_location_reader_t locations (*store, options);
    serializer_registry_t serializers;
    auto client = runtime::make_actor_client (locations, serializers, {source}, {}, options);
#if !defined(_WIN32)
    request_submissions.store (0);
    count_request_submissions.store (true);
#endif
    actor_request_call_t request (*client, actor_id_t (key.global_id), "AuthorityGeneration",
                                  message_t::from (std::string ("request")));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    auto pending = request.timeout (2s).async<std::string> ();
    const auto registration_deadline = std::chrono::steady_clock::now () + 250ms;
    while (source->native_node ().transport ().pending_operation_count () == 0
           && std::chrono::steady_clock::now () < registration_deadline)
        std::this_thread::yield ();
    assert (source->native_node ().transport ().pending_operation_count () == 1);
#if !defined(_WIN32)
    assert (request_submissions.load () == 0);
#endif
    if (invalidated)
        source->forget_peer (target_status.routing_id (), target_status.local_endpoint ());
    else
        assert (await_task (source->native_node ().transport ().expire_requests (deadline + 1s))
                == 1);
    const auto result = pending.result ();
    assert (!result);
    assert (result.error_kind ()
            == (invalidated ? framework_error_kind_t::unavailable
                            : framework_error_kind_t::deadline_exceeded));
    if (invalidated)
        assert (std::chrono::steady_clock::now () < deadline);
#if !defined(_WIN32)
    assert (request_submissions.load () == 0);
    count_request_submissions.store (false);
#endif
    assert (source->has_admitted_peer (target_status.routing_id (),
                                       target_status.lifecycle_generation ()));
    assert (!target->native_node ().transport ().mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 4096));
    source->stop ();
    target->stop ();
}

void verify_actor_calls_keep_selected_route_until_follow_notice ()
{
    using namespace zlink::framework;

    auto store = std::make_shared<counting_location_repository_t> ();
    const auto old_owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("actor-send-old-owner", 30s).result ().value ())
        .token;
    const auto new_owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("actor-send-new-owner", 30s).result ().value ())
        .token;

    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_runtime = [core_context] (std::string rid) {
        auto state = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::mesh_node_builder_state_t> ("actor-send-route-mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (std::move (rid));
        state->core_context = core_context;
        return std::pair{state, std::make_shared<detail::mesh_node_runtime_t> (state)};
    };
    auto [source_state, source] = make_runtime ("actor-send-source");
    auto [old_state, old_target] = make_runtime ("actor-send-old-target");
    auto [new_state, new_target] = make_runtime ("actor-send-new-target");
    (void) source_state;
    (void) new_state;
    old_target->configure_spot_route_fence_resolver (
      [runtime = new_target.get ()] (zlink::routing_id_t, std::string, std::uint64_t)
        -> zlink::framework::task_t<std::optional<host::route_fence_t>> {
          co_return host::route_fence_t{1, 1, runtime->status ().lifecycle_generation ()};
      });
    new_target->configure_stateful_dispatch (
      [] (stateful::accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{{"actor-send-source-owner", 1,
                                                           query.source_node_routing_id,
                                                           query.source_node_generation},
                                                          1};
      });
    old_target->start ().result ().value ();
    new_target->start ().result ().value ();
    auto target_spot = new_target->get_or_create_spot_async ("actor-send-spot").result ().value ();

    const auto publish_target = [&] (const detail::mesh_node_runtime_t &runtime,
                                     const location_owner_token_t &owner) {
        const auto status = runtime.status ();
        mesh_node_descriptor_t node{
          .mesh_name = "actor-send-route-mesh",
          .rid = status.routing_id (),
          .lifecycle_generation = status.lifecycle_generation (),
          .descriptor_revision = 1,
          .endpoint = status.local_endpoint (),
          .application_version = 1,
          .object_capabilities = {{.object_kind = placement_object_kind_t::actor,
                                   .stable_type = "route-probe"}},
          .object_role = object_role_t::server,
          .capacity = {.actors = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "actor-send-route-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store->update_mesh_node (std::move (node), location_write_intent_t::new_claim)
                  .result ()
                  .value ()
                  .status
                == location_write_status_t::stored);
    };
    publish_target (*old_target, old_owner);
    publish_target (*new_target, new_owner);

    source->configure_user_spot_operations (
      store,
      [] (const runtime::stateful::object_ref_t &, const std::string &,
          const std::vector<std::byte> &)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          co_return host::user_spot_materialize_result_t{true, std::nullopt};
      });
    std::atomic<std::size_t> invalidated_route_count{0};
    std::mutex invalidated_route_mutex;
    std::optional<protocol::actor_route_fence_t> invalidated_route;
    source->configure_actor_route_resolver (
      [] (actor_ref_t) -> task_t<std::optional<runtime::spot_address_t>> {
          co_return std::nullopt;
      },
      [&] (const protocol::actor_route_fence_t &route) {
          {
              std::lock_guard lock (invalidated_route_mutex);
              invalidated_route = route;
          }
          invalidated_route_count.fetch_add (1, std::memory_order_release);
      });
    source->start ().result ().value ();
    const auto source_status = source->status ();
    const auto old_status = old_target->status ();
    const auto new_status = new_target->status ();
    source->connect_peer (old_status.routing_id (), old_status.local_endpoint (),
                          old_status.lifecycle_generation ());
    source->connect_peer (new_status.routing_id (), new_status.local_endpoint (),
                          new_status.lifecycle_generation ());
    old_target->connect_peer (new_status.routing_id (), new_status.local_endpoint (),
                              new_status.lifecycle_generation ());

    const auto spot_resolver_owner = std::make_shared<actor_send_spot_resolver_t> ();
    auto &spot_resolver = *spot_resolver_owner;
    spot_resolver.address =
      runtime::spot_address_t{"actor-send-route-mesh", new_status.routing_id (), "actor-send-spot",
                              target_spot.status ().lifecycle_generation ()};

    const auto discard = [] (const host::ready_record_t &, const host::receive_record_t &,
                             std::vector<zlink::message_t>, auto) {};
    const auto connected_deadline = std::chrono::steady_clock::now () + 5s;
    while (
      (!source->has_admitted_peer (old_status.routing_id (), old_status.lifecycle_generation ())
       || !source->has_admitted_peer (new_status.routing_id (), new_status.lifecycle_generation ())
       || !old_target->has_admitted_peer (source_status.routing_id (),
                                          source_status.lifecycle_generation ())
       || !new_target->has_admitted_peer (source_status.routing_id (),
                                          source_status.lifecycle_generation ())
       || !old_target->has_admitted_peer (new_status.routing_id (),
                                          new_status.lifecycle_generation ())
       || !new_target->has_admitted_peer (old_status.routing_id (),
                                          old_status.lifecycle_generation ()))
      && std::chrono::steady_clock::now () < connected_deadline) {
        (void) await_task (source->dispatch_ready (discard));
        (void) await_task (old_target->dispatch_ready (discard));
        (void) await_task (new_target->dispatch_ready (discard));
        std::this_thread::sleep_for (1ms);
    }
    assert (
      source->has_admitted_peer (old_status.routing_id (), old_status.lifecycle_generation ()));
    assert (
      source->has_admitted_peer (new_status.routing_id (), new_status.lifecycle_generation ()));
    assert (
      old_target->has_admitted_peer (new_status.routing_id (), new_status.lifecycle_generation ()));

    const object_creation_key_t key{placement_object_kind_t::actor, "actor-send-route-probe"};
    const object_creation_target_t old_placement{
      "actor-send-route-mesh", node_rid_t::from_string (old_status.routing_id ().to_string ()),
      old_status.lifecycle_generation (), old_owner};
    const object_creation_target_t new_placement{
      "actor-send-route-mesh", node_rid_t::from_string (new_status.routing_id ().to_string ()),
      new_status.lifecycle_generation (), new_owner};
    const object_reserve_request_t reserve{.key = key,
                                           .intent = {.stable_type = "route-probe"},
                                           .target = old_placement,
                                           .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = std::get<object_reserved_t> (store->reserve (reserve).result ().value ());
    const auto old_actor = detail::actor_ref_access_t::make (
      old_placement.node_rid, "route-probe", key.global_id, reserved.fence.object_generation);
    const auto old_payload =
      runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
        .stable_type = "route-probe",
        .actor_id = key.global_id,
        .current_spot_id = old_target->native_node ().entry_spot ().spot_id (),
        .current_spot_generation = old_status.lifecycle_generation (),
        .current_spot_kind = runtime::actor_authority_spot_kind_t::entry,
        .owner_id = old_owner.owner_id,
        .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
        .mesh_name = old_placement.mesh_name,
        .node_rid = old_placement.node_rid,
        .node_generation = old_placement.node_lifecycle_generation});
    const auto committed = std::get<object_committed_t> (
      store->commit ({key, reserved.fence, old_payload}).result ().value ());

    location_options_t location_options;
    location_options.route_cache_max_age = 20s;
    location_options.owner_lease_fencing_margin = 1s;
    runtime::live_location_reader_t live_locations (*store, location_options);
    serializer_registry_t serializers;
    old_target->bind_serializers (serializers);
    new_target->bind_serializers (serializers);
    auto client =
      runtime::make_actor_client (live_locations, serializers, {source}, {}, location_options);

    const auto collect_actor_sends = [] (detail::mesh_node_runtime_t &target,
                                         std::chrono::milliseconds budget,
                                         std::size_t expected_count = 1) {
        std::size_t count = 0;
        const auto deadline = std::chrono::steady_clock::now () + budget;
        do {
            const auto pumped = target.native_node ()
                                  .transport ()
                                  .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                                  .result ()
                                  .value ();
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
            while (auto claim = target.native_node ().transport ().mailbox ().try_claim (
                     mesh::service_mailbox_domain_t::application, 16, 1024 * 1024)) {
                for (const auto &slot : claim->records) {
                    const auto &record = *slot;
                    if (protocol::decode_header (protocol::message_bytes (record.parts ().front ()))
                          .kind
                        == protocol::command::actorSend) {
                        ++count;
                    }
                }
                assert (target.native_node ().transport ().mailbox ().release (*claim));
            }
            if (count >= expected_count)
                break;
            std::this_thread::sleep_for (1ms);
        } while (std::chrono::steady_clock::now () < deadline);
        return count;
    };

    auto first =
      client->send (actor_id_t (key.global_id), std::string ("first")).async ().result ();
    assert (first);
    assert (collect_actor_sends (*old_target, 2s) == 1);
    assert (collect_actor_sends (*new_target, 20ms) == 0);

    // A second application scope can resolve the same singleton MeshNode
    // after this client has already cached A. Its notice registration cannot
    // replace the first client's exact-fence invalidation subscription.
    auto second_client =
      runtime::make_actor_client (live_locations, serializers, {source}, {}, location_options);

    const auto new_actor = detail::actor_ref_access_t::make (
      new_placement.node_rid, "route-probe", key.global_id, committed.ready.object_generation);
    const auto new_payload =
      runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
        .stable_type = "route-probe",
        .actor_id = key.global_id,
        .current_spot_id = new_target->native_node ().entry_spot ().spot_id (),
        .current_spot_generation = new_status.lifecycle_generation (),
        .current_spot_kind = runtime::actor_authority_spot_kind_t::entry,
        .owner_id = new_owner.owner_id,
        .owner_lease_generation = static_cast<std::uint64_t> (new_owner.lease_generation),
        .mesh_name = new_placement.mesh_name,
        .node_rid = new_placement.node_rid,
        .node_generation = new_placement.node_lifecycle_generation});
    const auto moved =
      std::get<authority_stored_t> (
        store
          ->compare_exchange_authority (runtime::actor_authority_key (key.global_id),
                                        committed.ready.store_version,
                                        authority_retarget_t{new_payload, new_placement})
          .result ()
          .value ())
        .snapshot;
    assert (moved.authority_owner_generation != committed.ready.authority_owner_generation);

    const auto source_fence =
      protocol::actor_route_fence_t{key.global_id,
                                    old_actor.object_generation (),
                                    old_status.routing_id ().to_bytes (),
                                    old_status.lifecycle_generation (),
                                    committed.ready.authority_owner_generation,
                                    static_cast<std::uint64_t> (old_owner.lease_generation)};
    const auto target_fence =
      protocol::actor_route_fence_t{key.global_id,
                                    new_actor.object_generation (),
                                    new_status.routing_id ().to_bytes (),
                                    new_status.lifecycle_generation (),
                                    moved.authority_owner_generation,
                                    static_cast<std::uint64_t> (new_owner.lease_generation)};

    old_state->spot_state->lane
      .run_checked (
        [&] { old_state->spot_state->actor_types_by_id[key.global_id] = "route-probe"; })
      .get ();
    detail::spot_node_runtime_t old_spots (old_state->spot_state);
    old_spots.bind_spot_location_resolver (spot_resolver_owner);
    assert (old_spots
              .complete_remote_actor_transfer (
                old_actor, new_actor,
                spot_route_t{new_placement.node_rid, spot_id_t ("actor-send-spot"),
                             "actor-send-route-mesh"},
                source_fence,
                {target_fence.target_node_routing_id,
                 target_fence.target_node_generation,
                 {},
                 target_fence.owner_lease_generation},
                target_fence.authority_owner_generation, "actor-send-follow-transfer")
              .result ());

    // The source Location reader can still hold the pre-commit authority
    // snapshot when the first post-relocation call arrives. The exact
    // Message Follow source fence is newer local knowledge and must win over
    // that stale positive cache result.
    std::atomic<std::size_t> stale_admission_reads{0};
    old_state->spot_state->actor_route_admission =
      [&] (const protocol::actor_route_fence_t &route) {
          assert (route == source_fence);
          stale_admission_reads.fetch_add (1, std::memory_order_relaxed);
          return true;
      };

    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    std::optional<protocol::wire_operation_id_t> dispatching_operation;
    std::optional<protocol::wire_operation_id_t> request_operation;
    std::atomic<std::size_t> request_deliveries{0};
    std::atomic<std::size_t> one_way_deliveries{0};
    old_spots.on_actor_message_follow (
      [&] (const actor_ref_t &actor, const runtime::messaging::envelope_header_t &header,
           const zlink::message_t &payload, std::chrono::milliseconds timeout,
           const zlink::routing_id_t &source_node, const protocol::actor_route_fence_t &route,
           std::uint8_t hop_count, const protocol::wire_operation_id_t &operation,
           std::uint64_t reply_route_id) -> task_t<std::optional<zlink::message_t>> {
          assert (actor.actor_id ().value () == key.global_id);
          assert (route == source_fence);
          assert (hop_count == 0);
          assert (source_node == source_status.routing_id ());
          assert (dispatching_operation && *dispatching_operation == operation);
          assert (operation.high != 0 || operation.low != 0);
          if (header.message_name == "B6Request") {
              request_operation = operation;
          }
          return old_target->relay_application_actor (actor, header, payload, timeout, source_node,
                                                      route, hop_count, operation, reply_route_id);
      });

    std::atomic<bool> stop_target_dispatch{false};
    std::thread target_dispatch ([&] {
        while (!stop_target_dispatch.load (std::memory_order_acquire)) {
            (void) await_task (new_target->dispatch_ready (
              [&] (const host::ready_record_t &ready, const host::receive_record_t &record,
                   std::vector<zlink::message_t> parts, auto) {
                  assert (ready.owner_kind == host::owner_kind_t::spot);
                  assert (ready.spot_id == "actor-send-spot");
                  assert (record.kind == host::record_kind_t::spot_request);
                  runtime::messaging::message_parts_t encoded (std::move (parts));
                  runtime::messaging::envelope_codec_t codec;
                  const auto header = codec.decode_header (encoded);
                  const auto body = codec.decode_body (encoded);
                  assert (header);
                  assert (body);
                  assert (header.value ().message_name == "__zlink.spot.actor.packet");
                  const auto request = nlohmann::json::parse (
                    detail::encoded_payload_from_raw (body.value ()).to_string ());
                  assert (request.at ("actorId").get<std::string> () == key.global_id);
                  assert (request.at ("actorNodeRid").get<std::string> ()
                          == new_actor.node_rid ().value ());
                  assert (request.at ("actorGeneration").get<std::uint64_t> ()
                          == new_actor.object_generation ());
                  assert (request.at ("messageFollowHopCount").get<std::uint8_t> () == 1);
                  const auto packet_name = request.at ("packetName").get<std::string> ();
                  if (packet_name == "B6Request") {
                      request_deliveries.fetch_add (1, std::memory_order_relaxed);
                  } else {
                      assert (packet_name == "B7OneWay");
                      one_way_deliveries.fetch_add (1, std::memory_order_relaxed);
                  }
                  const bool has_reply = packet_name == "B6Request";
                  const auto reply_body = nlohmann::json{
                    {"actorRefPresent", true},
                    {"actorNodeRid", std::string (new_actor.node_rid ().value ())},
                    {"actorType", std::string (detail::actor_ref_access_t::actor_type (new_actor))},
                    {"actorId", std::string (new_actor.actor_id ().value ())},
                    {"actorGeneration", new_actor.object_generation ()},
                    {"hasReply", has_reply},
                    {"payload", has_reply
                                  ? "ImZvbGxvdy1yZXBseSI="
                                  : ""}}.dump ();
                  detail::channel_reply_writer_t replies;
                  auto reply = replies.reply_raw_envelope (
                    replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                                 header.value ().channel_name, header.value ()),
                    detail::encoded_payload_to_raw (encoded_payload_t::from_string (reply_body)));
                  assert (host::reply (record.reply_token, reply.items ())
                          == zlink::submit_result_t::ok);
              }));
            std::this_thread::sleep_for (1ms);
        }
    });

    std::future<bool> old_dispatch_future;
    const auto dispatch_follow = [&] (const host::ready_record_t &ready,
                                      const host::receive_record_t &record,
                                      std::vector<zlink::message_t> parts, auto permit) {
        if (record.kind == host::record_kind_t::completion)
            return;
        assert (record.actor_route);
        assert (!old_dispatch_future.valid ());
        dispatching_operation =
          protocol::wire_operation_id_t{record.operation_id.high, record.operation_id.low};
        auto completion = std::make_shared<std::promise<bool>> ();
        old_dispatch_future = completion->get_future ();
        bool terminal_deferred = false;
        const auto accepted = old_spots.dispatch_mesh_record (
          ready, record, parts,
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
          provider, serializers, [completion] { completion->set_value (true); }, &terminal_deferred,
          zlink::framework::runtime::handler_entry_t (record.before_application_handler,
                                                      std::move (permit)));
        if (!accepted || !terminal_deferred)
            completion->set_value (accepted);
    };
    const auto wait_for_application = [] (detail::mesh_node_runtime_t &target,
                                          std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now () + budget;
        do {
            const auto pumped = target.native_node ()
                                  .transport ()
                                  .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                                  .result ()
                                  .value ();
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
            if (target.native_node ().transport ().mailbox ().pending_messages (
                  mesh::service_mailbox_domain_t::application)
                != 0) {
                return true;
            }
            std::this_thread::sleep_for (1ms);
        } while (std::chrono::steady_clock::now () < deadline);
        return false;
    };

    auto request_future = std::async (std::launch::async, [&] {
        actor_request_call_t request (*client, actor_id_t (key.global_id), "B6Request",
                                      message_t::from (std::string ("request")));
        return request.timeout (2s).async<std::string> ().result ();
    });
    const auto request_deadline = std::chrono::steady_clock::now () + 2s;
    while ((!old_dispatch_future.valid ()
            || old_dispatch_future.wait_for (0ms) != std::future_status::ready)
           && std::chrono::steady_clock::now () < request_deadline) {
        (void) await_task (old_target->dispatch_ready (dispatch_follow));
        std::this_thread::sleep_for (1ms);
    }
    assert (old_dispatch_future.valid ());
    assert (old_dispatch_future.wait_for (0ms) == std::future_status::ready);
    assert (old_dispatch_future.get ());
    dispatching_operation.reset ();
    assert (stale_admission_reads.load (std::memory_order_relaxed) != 0);
    assert (request_deliveries.load (std::memory_order_relaxed) == 1);
    assert (request_operation);
    assert (invalidated_route_count.load (std::memory_order_acquire) == 0);

    actor_send_call_t one_way (*client, actor_id_t (key.global_id), "B7OneWay",
                               message_t::from (std::string ("move-48")));
    assert (one_way.async ().result ());
    const auto one_way_deadline = std::chrono::steady_clock::now () + 2s;
    while ((!old_dispatch_future.valid ()
            || old_dispatch_future.wait_for (0ms) != std::future_status::ready
            || one_way_deliveries.load (std::memory_order_acquire) == 0)
           && std::chrono::steady_clock::now () < one_way_deadline) {
        (void) await_task (old_target->dispatch_ready (dispatch_follow));
        std::this_thread::sleep_for (1ms);
    }
    assert (old_dispatch_future.valid ());
    assert (old_dispatch_future.wait_for (0ms) == std::future_status::ready);
    assert (old_dispatch_future.get ());
    dispatching_operation.reset ();
    assert (one_way_deliveries.load (std::memory_order_relaxed) == 1);
    assert (invalidated_route_count.load (std::memory_order_acquire) == 0);

    // Once the caller applies the exact notice, only a later application call
    // resolves the current owner directly.
    const auto notice_deadline = std::chrono::steady_clock::now () + 2s;
    while ((invalidated_route_count.load (std::memory_order_acquire) == 0
            || request_future.wait_for (0ms) != std::future_status::ready)
           && std::chrono::steady_clock::now () < notice_deadline) {
        (void) await_task (source->dispatch_ready (discard));
        std::this_thread::sleep_for (1ms);
    }
    assert (invalidated_route_count.load (std::memory_order_acquire) == 1);
    assert (request_future.wait_for (0ms) == std::future_status::ready);
    const auto request_result = request_future.get ();
    assert (request_result);
    assert (request_result.value () == "follow-reply");
    {
        std::lock_guard lock (invalidated_route_mutex);
        assert (invalidated_route == source_fence);
    }
    stop_target_dispatch.store (true, std::memory_order_release);
    target_dispatch.join ();
    assert (invalidated_route_count.load (std::memory_order_acquire) == 1);

    actor_send_call_t after_notice (*client, actor_id_t (key.global_id), "AfterB7FollowNotice",
                                    message_t::from (std::string ("after-notice")));
    assert (after_notice.async ().result ());
    assert (wait_for_application (*new_target, 2s));
    assert (!wait_for_application (*old_target, 20ms));
    assert (one_way_deliveries.load (std::memory_order_relaxed) == 1);

    actor_send_call_t second_prime (*second_client, actor_id_t (key.global_id), "SecondClientPrime",
                                    message_t::from (std::string ("prime-b")));
    assert (second_prime.async ().result ());

    const auto returned = std::get<authority_stored_t> (
                            store
                              ->compare_exchange_authority (
                                runtime::actor_authority_key (key.global_id), moved.store_version,
                                authority_retarget_t{old_payload, old_placement})
                              .result ()
                              .value ())
                            .snapshot;

    // A late duplicate notice for the retired source fence cannot invalidate
    // the successor B route selected by B7. Destroy the first client before
    // dispatch so its exact unregister cannot remove the remaining client's
    // subscription.
    client.reset ();
    source->dispatch_message_follow (
      protocol::message_follow_notice_t{.source = source_fence,
                                        .target = target_fence,
                                        .hop_count = 1,
                                        .queued_messages = 1,
                                        .queued_bytes = 1,
                                        .original_operation = *request_operation,
                                        .original_reply_route_id = 0});

    const auto reads_before_late_call = store->authority_reads.load (std::memory_order_relaxed);
    actor_send_call_t later (*second_client, actor_id_t (key.global_id), "AfterFollowNotice",
                             message_t::from (std::string ("later")));
    assert (later.async ().result ());
    // A cached call performs only the deletion-only preflight read. A cache
    // miss would add a second authority read and select the returned A route.
    assert (store->authority_reads.load (std::memory_order_relaxed) - reads_before_late_call == 1);
    assert (collect_actor_sends (*new_target, 2s, 3) == 3);
    assert (collect_actor_sends (*old_target, 20ms) == 0);

    const auto returned_fence =
      protocol::actor_route_fence_t{key.global_id,
                                    old_actor.object_generation (),
                                    old_status.routing_id ().to_bytes (),
                                    old_status.lifecycle_generation (),
                                    returned.authority_owner_generation,
                                    static_cast<std::uint64_t> (old_owner.lease_generation)};
    const auto matching_notice =
      protocol::message_follow_notice_t{.source = target_fence,
                                        .target = returned_fence,
                                        .hop_count = 1,
                                        .queued_messages = 1,
                                        .queued_bytes = 1,
                                        .original_operation = *request_operation};
    source->dispatch_message_follow (matching_notice);
    source->dispatch_message_follow (matching_notice);
    const auto reads_before_remaining_call =
      store->authority_reads.load (std::memory_order_relaxed);
    actor_send_call_t remaining (*second_client, actor_id_t (key.global_id),
                                 "RemainingClientAfterNotice",
                                 message_t::from (std::string ("remaining")));
    assert (remaining.async ().result ());
    assert (store->authority_reads.load (std::memory_order_relaxed) - reads_before_remaining_call
            == 2);
    assert (collect_actor_sends (*old_target, 2s) == 1);
    assert (collect_actor_sends (*new_target, 20ms) == 0);

    assert (old_spots.matches_actor_message_follow_source (old_actor, source_fence));
    assert (
      old_spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::time_point::max ())
      != 0);
    assert (!old_spots.matches_actor_message_follow_source (old_actor, source_fence));
    assert (
      old_spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::time_point::max ())
      == 0);

    // A request already admitted by the selected route keeps its completion
    // result even when the owner's lease is released before the reply deadline.
    auto timed_out_request = std::async (std::launch::async, [&] {
        actor_request_call_t request (*second_client, actor_id_t (key.global_id),
                                      "WithheldAfterOwnerRelease",
                                      message_t::from (std::string ("request")));
        return request.timeout (200ms).async<std::string> ().result ();
    });
    bool request_admitted = false;
    const auto admission_deadline = std::chrono::steady_clock::now () + 2s;
    while (!request_admitted && std::chrono::steady_clock::now () < admission_deadline) {
        const auto pumped = old_target->native_node ()
                              .transport ()
                              .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                              .result ()
                              .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = old_target->native_node ().transport ().mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application, 16, 1024 * 1024);
        if (claim) {
            for (const auto &slot : claim->records) {
                const auto &record = *slot;
                request_admitted |=
                  protocol::decode_header (protocol::message_bytes (record.parts ().front ())).kind
                  == protocol::command::actorRequest;
            }
            assert (old_target->native_node ().transport ().mailbox ().release (*claim));
        }
    }
    assert (request_admitted);
    (void) store->release_owner_lease (old_owner).result ().value ();
    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (timed_out_request.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) await_task (source->dispatch_ready (discard));
    }
    assert (timed_out_request.wait_for (0ms) == std::future_status::ready);
    const auto timed_out = timed_out_request.get ();
    assert (!timed_out);
    assert (timed_out.error_kind () == framework_error_kind_t::deadline_exceeded);

    source->stop ();
    second_client.reset ();
    old_target->stop ();
    new_target->stop ();
}
}
