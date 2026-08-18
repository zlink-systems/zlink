/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/actors/actor_client.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/route_mesh_connection_policy.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/source_creation_cleanup.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/utils/relocation_id_generator.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
namespace spots = zlink::framework::detail;

using namespace std::chrono_literals;

namespace
{

template<class T>
T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

mesh::service_node_descriptor_t descriptor (std::string rid);

zlink::framework::task_t<std::vector<zlink::message_t>>
await_native_reply (
  zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    co_return co_await std::move (pending);
}

class actor_send_spot_resolver_t final
    : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        co_return spot_id == address.spot_id
                    ? std::make_optional (address)
                    : std::nullopt;
    }

    void invalidate_spot_address (std::string_view) override {}

    void invalidate_all_routes_after_store_recovery () override {}

    zlink::framework::runtime::spot_address_t address;
};

class counting_location_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority (
      zlink::framework::authority_key_t key,
      std::stop_token cancellation = {}) override
    {
        authority_reads.fetch_add (1, std::memory_order_relaxed);
        return in_memory_location_repository_t::read_authority (
          std::move (key), cancellation);
    }

    std::atomic<std::size_t> authority_reads{0};
};

void verify_session_relocation_route_retry_cadence ()
{
    using schedule_t =
      host::relocation_detail::session_route_retry_schedule_t;
    schedule_t retry;
    const auto started_at =
      schedule_t::clock_t::time_point{std::chrono::seconds (100)};
    auto now = started_at;
    constexpr std::array expected{
      1s, 1s, 2s, 4s, 5s, 5s, 5s};
    constexpr std::array absolute_attempts{
      0s, 1s, 2s, 4s, 8s, 13s, 18s};

    assert (retry.due (now));
    for (std::size_t attempt = 0; attempt != expected.size (); ++attempt) {
        assert (now - started_at == absolute_attempts[attempt]);
        const auto timeout = retry.started (now);
        assert (timeout == expected[attempt]);
        assert (retry.attempts == attempt + 1);
        assert (!retry.due (now + expected[attempt] - 1ms));
        assert (retry.due (now + expected[attempt]));
        now += expected[attempt];
    }
}

void verify_message_follow_invalidation_subscriptions_are_lifetime_safe ()
{
    using namespace zlink::framework;

    auto state = std::make_shared<detail::mesh_node_builder_state_t> (
      "message-follow-subscription-mesh");
    auto runtime = std::make_shared<detail::mesh_node_runtime_t> (state);
    runtime->configure_actor_route_resolver (
      [] (const actor_ref_t &)
        -> std::optional<zlink::framework::runtime::spot_address_t> {
          return std::nullopt;
      },
      [] (const auto &) { throw std::runtime_error ("expected invalidator failure"); });
    const auto route = protocol::actor_route_fence_t{
      "subscription-actor", 1,
      zlink::routing_id_t::from ("subscription-source").to_bytes (),
      1, 1, 1};
    const auto notice = protocol::message_follow_notice_t{
      .source = route,
      .target = route,
      .original_operation = {1, 1}};

    std::atomic<std::size_t> duplicate_calls{0};
    const auto throwing = runtime->subscribe_message_follow_invalidation (
      [] (const auto &) { throw std::runtime_error ("expected subscriber failure"); });
    const auto duplicate_handler = [&] (const auto &) {
        duplicate_calls.fetch_add (1, std::memory_order_relaxed);
    };
    const auto first =
      runtime->subscribe_message_follow_invalidation (duplicate_handler);
    const auto second =
      runtime->subscribe_message_follow_invalidation (duplicate_handler);
    assert (first != 0 && second != 0 && first != second);
    runtime->dispatch_message_follow (notice);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 2);
    runtime->unsubscribe_message_follow_invalidation (throwing);

    runtime->unsubscribe_message_follow_invalidation (second);
    runtime->unsubscribe_message_follow_invalidation (second);
    runtime->dispatch_message_follow (notice);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 3);

    std::mutex callback_mutex;
    std::condition_variable callback_changed;
    bool callback_entered = false;
    bool callback_release = false;
    std::atomic<std::size_t> blocking_calls{0};
    const auto blocking = runtime->subscribe_message_follow_invalidation (
      [&] (const auto &) {
          blocking_calls.fetch_add (1, std::memory_order_relaxed);
          std::unique_lock lock (callback_mutex);
          callback_entered = true;
          callback_changed.notify_all ();
          callback_changed.wait (lock, [&] { return callback_release; });
      });
    auto dispatch = std::async (
      std::launch::async,
      [&] { runtime->dispatch_message_follow (notice); });
    {
        std::unique_lock lock (callback_mutex);
        assert (callback_changed.wait_for (
          lock, 2s, [&] { return callback_entered; }));
    }
    auto unsubscribe = std::async (
      std::launch::async,
      [&] { runtime->unsubscribe_message_follow_invalidation (blocking); });
    assert (unsubscribe.wait_for (20ms) == std::future_status::timeout);
    {
        std::lock_guard lock (callback_mutex);
        callback_release = true;
    }
    callback_changed.notify_all ();
    assert (dispatch.wait_for (2s) == std::future_status::ready);
    dispatch.get ();
    assert (unsubscribe.wait_for (2s) == std::future_status::ready);
    unsubscribe.get ();

    runtime->dispatch_message_follow (notice);
    assert (blocking_calls.load (std::memory_order_relaxed) == 1);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 5);

    detail::mesh_node_runtime_t::message_follow_subscription_id_t self = 0;
    std::atomic<std::size_t> self_calls{0};
    self = runtime->subscribe_message_follow_invalidation (
      [&] (const auto &) {
          self_calls.fetch_add (1, std::memory_order_relaxed);
          runtime->unsubscribe_message_follow_invalidation (self);
      });
    runtime->dispatch_message_follow (notice);
    runtime->dispatch_message_follow (notice);
    assert (self_calls.load (std::memory_order_relaxed) == 1);

    auto second_state = std::make_shared<detail::mesh_node_builder_state_t> (
      "message-follow-subscription-second-mesh");
    auto second_runtime =
      std::make_shared<detail::mesh_node_runtime_t> (second_state);
    std::atomic<std::size_t> multi_mesh_calls{0};
    const auto multi_first =
      runtime->subscribe_message_follow_invalidation (
        [&] (const auto &) {
            multi_mesh_calls.fetch_add (1, std::memory_order_relaxed);
        });
    const auto multi_second =
      second_runtime->subscribe_message_follow_invalidation (
        [&] (const auto &) {
            multi_mesh_calls.fetch_add (1, std::memory_order_relaxed);
        });
    runtime->dispatch_message_follow (notice);
    second_runtime->dispatch_message_follow (notice);
    assert (multi_mesh_calls.load (std::memory_order_relaxed) == 2);
    runtime->unsubscribe_message_follow_invalidation (multi_first);
    second_runtime->unsubscribe_message_follow_invalidation (multi_second);
    runtime->unsubscribe_message_follow_invalidation (first);
}

void verify_actor_calls_keep_selected_route_until_follow_notice ()
{
    using namespace zlink::framework;

    auto store = std::make_shared<counting_location_repository_t> ();
    const auto old_owner = std::get<owner_lease_claimed_t> (
      store->claim_owner_lease ("actor-send-old-owner", 30s)
        .result ().value ()).token;
    const auto new_owner = std::get<owner_lease_claimed_t> (
      store->claim_owner_lease ("actor-send-new-owner", 30s)
        .result ().value ()).token;

    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_runtime = [core_context] (std::string rid) {
        auto state = std::make_shared<detail::mesh_node_builder_state_t> (
          "actor-send-route-mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (std::move (rid));
        state->core_context = core_context;
        return std::pair{
          state,
          std::make_shared<detail::mesh_node_runtime_t> (state)};
    };
    auto [source_state, source] = make_runtime ("actor-send-source");
    auto [old_state, old_target] = make_runtime ("actor-send-old-target");
    auto [new_state, new_target] = make_runtime ("actor-send-new-target");
    (void) source_state;
    (void) new_state;
    old_target->configure_spot_route_fence_resolver (
      [] (const zlink::routing_id_t &, std::string_view,
          std::uint64_t)
        -> std::optional<host::route_fence_t> {
          return host::route_fence_t{1, 1};
      },
      1min);
    new_target->configure_stateful_dispatch (
      [] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          return stateful::accepted_record_authority_t{
            {"actor-send-source-owner", 1,
             query.source_node_routing_id,
             query.source_node_generation},
            1};
      });
    old_target->start ();
    new_target->start ();
    auto target_spot =
      new_target->get_or_create_spot ("actor-send-spot");

    const auto publish_target = [&] (
      const detail::mesh_node_runtime_t &runtime,
      const location_owner_token_t &owner) {
        const auto status = runtime.status ();
        mesh_node_descriptor_t node{
          .mesh_name = "actor-send-route-mesh",
          .rid = status.routing_id (),
          .lifecycle_generation = status.lifecycle_generation (),
          .descriptor_revision = 1,
          .endpoint = status.local_endpoint (),
          .application_version = 1,
          .object_capabilities =
            {{.object_kind = placement_object_kind_t::actor,
              .stable_type = "route-probe"}},
          .object_role = object_role_t::server,
          .capacity = {.actors = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "actor-send-route-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store->update_mesh_node (
                  std::move (node),
                  location_write_intent_t::new_claim)
                  .result ().value ().status
                == location_write_status_t::stored);
    };
    publish_target (*old_target, old_owner);
    publish_target (*new_target, new_owner);

    source->configure_user_spot_operations (
      store,
      [] (const runtime::stateful::object_ref_t &,
          const std::string &,
          const std::vector<std::byte> &) {
          return host::user_spot_materialize_result_t{
            true, std::nullopt};
      });
    std::atomic<std::size_t> invalidated_route_count{0};
    std::mutex invalidated_route_mutex;
    std::optional<protocol::actor_route_fence_t> invalidated_route;
    source->configure_actor_route_resolver (
      [] (const actor_ref_t &)
        -> std::optional<runtime::spot_address_t> {
          return std::nullopt;
      },
      [&] (const protocol::actor_route_fence_t &route) {
          {
              std::lock_guard lock (invalidated_route_mutex);
              invalidated_route = route;
          }
          invalidated_route_count.fetch_add (1, std::memory_order_release);
      });
    source->start ();
    const auto source_status = source->status ();
    const auto old_status = old_target->status ();
    const auto new_status = new_target->status ();
    source->connect_peer (
      old_status.routing_id (), old_status.local_endpoint (),
      old_status.lifecycle_generation ());
    source->connect_peer (
      new_status.routing_id (), new_status.local_endpoint (),
      new_status.lifecycle_generation ());
    old_target->connect_peer (
      new_status.routing_id (), new_status.local_endpoint (),
      new_status.lifecycle_generation ());

    actor_send_spot_resolver_t spot_resolver;
    spot_resolver.address = runtime::spot_address_t{
      "actor-send-route-mesh",
      new_status.routing_id (),
      "actor-send-spot",
      target_spot.status ().lifecycle_generation ()};

    const auto discard = [] (const host::ready_record_t &,
                             const host::receive_record_t &,
                             std::vector<zlink::message_t>) {};
    const auto connected_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source->has_admitted_peer (
              old_status.routing_id (),
              old_status.lifecycle_generation ())
            || !source->has_admitted_peer (
              new_status.routing_id (),
              new_status.lifecycle_generation ())
            || !old_target->has_admitted_peer (
              source_status.routing_id (),
              source_status.lifecycle_generation ())
            || !new_target->has_admitted_peer (
              source_status.routing_id (),
              source_status.lifecycle_generation ())
            || !old_target->has_admitted_peer (
              new_status.routing_id (),
              new_status.lifecycle_generation ())
            || !new_target->has_admitted_peer (
              old_status.routing_id (),
              old_status.lifecycle_generation ()))
           && std::chrono::steady_clock::now ()
                < connected_deadline) {
        (void) await_task (source->dispatch_ready (discard));
        (void) await_task (old_target->dispatch_ready (discard));
        (void) await_task (new_target->dispatch_ready (discard));
        std::this_thread::sleep_for (1ms);
    }
    assert (source->has_admitted_peer (
      old_status.routing_id (), old_status.lifecycle_generation ()));
    assert (source->has_admitted_peer (
      new_status.routing_id (), new_status.lifecycle_generation ()));
    assert (old_target->has_admitted_peer (
      new_status.routing_id (), new_status.lifecycle_generation ()));

    const object_creation_key_t key{
      placement_object_kind_t::actor, "actor-send-route-probe"};
    const object_creation_target_t old_placement{
      "actor-send-route-mesh",
      node_rid_t::from_string (old_status.routing_id ().to_string ()),
      old_status.lifecycle_generation (), old_owner};
    const object_creation_target_t new_placement{
      "actor-send-route-mesh",
      node_rid_t::from_string (new_status.routing_id ().to_string ()),
      new_status.lifecycle_generation (), new_owner};
    const object_reserve_request_t reserve{
      .key = key,
      .intent = {.stable_type = "route-probe"},
      .target = old_placement,
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = std::get<object_reserved_t> (
      store->reserve (reserve).result ().value ());
    const auto old_actor = detail::actor_ref_access_t::make (
      old_placement.node_rid, "route-probe", key.global_id,
      reserved.fence.object_generation);
    const auto committed = std::get<object_committed_t> (
      store->commit (
        {key, reserved.fence,
         runtime::encode_actor_authority_payload (
           old_actor, "actor-send-spot", 1)})
        .result ().value ());

    location_options_t location_options;
    location_options.route_cache_max_age = 20s;
    location_options.owner_lease_fencing_margin = 1s;
    runtime::live_location_reader_t live_locations (
      *store, location_options);
    serializer_registry_t serializers;
    old_target->bind_serializers (serializers);
    new_target->bind_serializers (serializers);
    auto client = runtime::make_actor_client (
      live_locations, serializers, {source}, {}, location_options);

    const auto collect_actor_sends = [] (
      detail::mesh_node_runtime_t &target,
      std::chrono::milliseconds budget,
      std::size_t expected_count = 1) {
        std::size_t count = 0;
        const auto deadline =
          std::chrono::steady_clock::now () + budget;
        do {
            const auto pumped =
              target.native_node ().transport ().pump_one (
                mesh::service_liveness_registry_t::clock_t::now ())
                .result ()
                .value ();
            assert (pumped
                    != mesh::raw_mesh_pump_result_t::protocol_error);
            while (auto claim =
                     target.native_node ().transport ().mailbox ().try_claim (
                       mesh::service_mailbox_domain_t::application,
                       16, 1024 * 1024)) {
                for (const auto &record : claim->records) {
                    if (protocol::decode_header (
                          record.parts.front ()).kind
                        == protocol::command::actorSend) {
                        ++count;
                    }
                }
                assert (target.native_node ().transport ().mailbox ().release (
                  *claim));
            }
            if (count >= expected_count)
                break;
            std::this_thread::sleep_for (1ms);
        } while (std::chrono::steady_clock::now () < deadline);
        return count;
    };

    auto first = client->send (
      actor_id_t (key.global_id), std::string ("first")).submit ().result ();
    assert (first);
    assert (collect_actor_sends (*old_target, 2s) == 1);
    assert (collect_actor_sends (*new_target, 20ms) == 0);

    // A second application scope can resolve the same singleton MeshNode
    // after this client has already cached A. Its notice registration cannot
    // replace the first client's exact-fence invalidation subscription.
    auto second_client = runtime::make_actor_client (
      live_locations, serializers, {source}, {}, location_options);

    const auto new_actor = detail::actor_ref_access_t::make (
      new_placement.node_rid, "route-probe", key.global_id,
      committed.ready.object_generation);
    const auto moved = std::get<authority_stored_t> (
      store->compare_exchange_authority (
        runtime::actor_authority_key (key.global_id),
        committed.ready.store_version,
        authority_retarget_t{
          runtime::encode_actor_authority_payload (
            new_actor, "actor-send-spot", 1),
          new_placement})
        .result ().value ()).snapshot;
    assert (moved.authority_owner_generation
            != committed.ready.authority_owner_generation);

    const auto source_fence = protocol::actor_route_fence_t{
      key.global_id,
      old_actor.object_generation (),
      old_status.routing_id ().to_bytes (),
      old_status.lifecycle_generation (),
      committed.ready.authority_owner_generation,
      static_cast<std::uint64_t> (old_owner.lease_generation)};
    const auto target_fence = protocol::actor_route_fence_t{
      key.global_id,
      new_actor.object_generation (),
      new_status.routing_id ().to_bytes (),
      new_status.lifecycle_generation (),
      moved.authority_owner_generation,
      static_cast<std::uint64_t> (new_owner.lease_generation)};

    {
        std::lock_guard<std::recursive_mutex> lock (
          old_state->spot_state->mutex);
        old_state->spot_state->actor_types_by_id[key.global_id] =
          "route-probe";
    }
    detail::spot_node_runtime_t old_spots (old_state->spot_state);
    old_spots.bind_spot_location_resolver (spot_resolver);
    assert (old_spots.complete_remote_actor_transfer (
      old_actor,
      new_actor,
      spot_route_t{new_placement.node_rid,
                   spot_id_t ("actor-send-spot"),
                   "actor-send-route-mesh"},
      source_fence,
      target_fence,
      "actor-send-follow-transfer")
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
      [&] (const actor_ref_t &actor,
           const runtime::messaging::envelope_header_t &header,
           const zlink::message_t &payload,
           std::chrono::milliseconds timeout,
           const zlink::routing_id_t &source_node,
           const protocol::actor_route_fence_t &route,
           std::uint8_t hop_count,
           const protocol::wire_operation_id_t &operation,
           std::uint64_t reply_route_id)
        -> task_t<std::optional<zlink::message_t>> {
          assert (actor.actor_id ().value () == key.global_id);
          assert (route == source_fence);
          assert (hop_count == 0);
          assert (source_node == source_status.routing_id ());
          assert (dispatching_operation
                  && *dispatching_operation == operation);
          assert (operation.high != 0 || operation.low != 0);
          if (header.message_name == "B6Request") {
              request_operation = operation;
          }
          return old_target->relay_application_actor (
            actor, header, payload, timeout, source_node, route,
            hop_count, operation, reply_route_id);
      });

    std::atomic<bool> stop_target_dispatch{false};
    std::thread target_dispatch ([&] {
        while (!stop_target_dispatch.load (std::memory_order_acquire)) {
            (void) await_task (new_target->dispatch_ready (
              [&] (const host::ready_record_t &ready,
                   const host::receive_record_t &record,
                   std::vector<zlink::message_t> parts) {
                  assert (ready.owner_kind == host::owner_kind_t::spot);
                  assert (ready.spot_id == "actor-send-spot");
                  assert (record.kind == host::record_kind_t::spot_request);
                  runtime::messaging::message_parts_t encoded (
                    std::move (parts));
                  runtime::messaging::envelope_codec_t codec;
                  const auto header = codec.decode_header (encoded);
                  const auto body = codec.decode_body (encoded);
                  assert (header);
                  assert (body);
                  assert (header.value ().message_name
                          == "__zlink.spot.actor.packet");
                  const auto request = nlohmann::json::parse (
                    detail::encoded_payload_from_raw (
                      body.value ()).to_string ());
                  assert (request.at ("actorId").get<std::string> ()
                          == key.global_id);
                  assert (request.at ("actorNodeRid").get<std::string> ()
                          == new_actor.node_rid ().value ());
                  assert (request.at ("actorGeneration").get<std::uint64_t> ()
                          == new_actor.object_generation ());
                  assert (request.at ("messageFollowHopCount")
                            .get<std::uint8_t> () == 1);
                  const auto packet_name =
                    request.at ("packetName").get<std::string> ();
                  if (packet_name == "B6Request") {
                      request_deliveries.fetch_add (
                        1, std::memory_order_relaxed);
                  } else {
                      assert (packet_name == "B7OneWay");
                      one_way_deliveries.fetch_add (
                        1, std::memory_order_relaxed);
                  }
                  const bool has_reply =
                    packet_name == "B6Request";
                  const auto reply_body =
                    nlohmann::json{
                      {"actorRefPresent", true},
                      {"actorNodeRid",
                       std::string (new_actor.node_rid ().value ())},
                      {"actorType",
                       std::string (
                         detail::actor_ref_access_t::actor_type (
                           new_actor))},
                      {"actorId",
                       std::string (new_actor.actor_id ().value ())},
                      {"actorGeneration",
                       new_actor.object_generation ()},
                      {"hasReply", has_reply},
                      {"payload",
                       has_reply ? "ImZvbGxvdy1yZXBseSI=" : ""}}
                      .dump ();
                  detail::channel_reply_writer_t replies;
                  auto reply = replies.reply_raw_envelope (
                    replies.create_reply_header (
                      runtime::messaging::message_kind_t::response,
                      header.value ().channel_name, header.value ()),
                    detail::encoded_payload_to_raw (
                      encoded_payload_t::from_string (reply_body)));
                  assert (host::reply (
                            record.reply_token, reply.items ())
                          == zlink::submit_result_t::ok);
              }));
            std::this_thread::sleep_for (1ms);
        }
    });

    std::future<bool> old_dispatch_future;
    const auto dispatch_follow = [&] (
      const host::ready_record_t &ready,
      const host::receive_record_t &record,
      std::vector<zlink::message_t> parts) {
        if (record.kind == host::record_kind_t::completion)
            return;
        assert (record.actor_route);
        assert (!old_dispatch_future.valid ());
        dispatching_operation = protocol::wire_operation_id_t{
          record.operation_id.high, record.operation_id.low};
        auto completion = std::make_shared<std::promise<bool>> ();
        old_dispatch_future = completion->get_future ();
        bool terminal_deferred = false;
        const auto accepted = old_spots.dispatch_mesh_record (
          ready, record, parts, provider, serializers,
          [completion] { completion->set_value (true); },
          &terminal_deferred);
        if (!accepted || !terminal_deferred)
            completion->set_value (accepted);
    };
    const auto wait_for_application = [] (
      detail::mesh_node_runtime_t &target,
      std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now () + budget;
        do {
            const auto pumped =
              target.native_node ().transport ().pump_one (
                mesh::service_liveness_registry_t::clock_t::now ())
                .result ()
                .value ();
            assert (pumped
                    != mesh::raw_mesh_pump_result_t::protocol_error);
            if (target.native_node ().transport ().mailbox ().pending_messages (
                  mesh::service_mailbox_domain_t::application) != 0) {
                return true;
            }
            std::this_thread::sleep_for (1ms);
        } while (std::chrono::steady_clock::now () < deadline);
        return false;
    };

    auto request_future = std::async (std::launch::async, [&] {
        actor_request_call_t request (
          *client,
          actor_id_t (key.global_id),
          "B6Request",
          message_t::from (std::string ("request")));
        return request.timeout (2s).submit<std::string> ().result ();
    });
    const auto request_deadline = std::chrono::steady_clock::now () + 2s;
    while ((!old_dispatch_future.valid ()
            || old_dispatch_future.wait_for (0ms)
                 != std::future_status::ready)
           && std::chrono::steady_clock::now () < request_deadline) {
        (void) await_task (old_target->dispatch_ready (dispatch_follow));
        std::this_thread::sleep_for (1ms);
    }
    assert (old_dispatch_future.valid ());
    assert (old_dispatch_future.wait_for (0ms)
            == std::future_status::ready);
    assert (old_dispatch_future.get ());
    dispatching_operation.reset ();
    assert (stale_admission_reads.load (std::memory_order_relaxed) != 0);
    assert (request_deliveries.load (std::memory_order_relaxed) == 1);
    assert (request_operation);
    assert (invalidated_route_count.load (std::memory_order_acquire) == 0);

    actor_send_call_t one_way (
      *client,
      actor_id_t (key.global_id),
      "B7OneWay",
      message_t::from (std::string ("move-48")));
    assert (one_way.submit ().result ());
    const auto one_way_deadline = std::chrono::steady_clock::now () + 2s;
    while ((!old_dispatch_future.valid ()
            || old_dispatch_future.wait_for (0ms)
                 != std::future_status::ready
            || one_way_deliveries.load (std::memory_order_acquire) == 0)
           && std::chrono::steady_clock::now () < one_way_deadline) {
        (void) await_task (old_target->dispatch_ready (dispatch_follow));
        std::this_thread::sleep_for (1ms);
    }
    assert (old_dispatch_future.valid ());
    assert (old_dispatch_future.wait_for (0ms)
            == std::future_status::ready);
    assert (old_dispatch_future.get ());
    dispatching_operation.reset ();
    assert (one_way_deliveries.load (std::memory_order_relaxed) == 1);
    assert (invalidated_route_count.load (std::memory_order_acquire) == 0);

    // Once the caller applies the exact notice, only a later application call
    // resolves the current owner directly.
    const auto notice_deadline = std::chrono::steady_clock::now () + 2s;
    while ((invalidated_route_count.load (std::memory_order_acquire) == 0
            || request_future.wait_for (0ms)
                 != std::future_status::ready)
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

    actor_send_call_t after_notice (
      *client,
      actor_id_t (key.global_id),
      "AfterB7FollowNotice",
      message_t::from (std::string ("after-notice")));
    assert (after_notice.submit ().result ());
    assert (wait_for_application (*new_target, 2s));
    assert (!wait_for_application (*old_target, 20ms));
    assert (one_way_deliveries.load (std::memory_order_relaxed) == 1);

    actor_send_call_t second_prime (
      *second_client,
      actor_id_t (key.global_id),
      "SecondClientPrime",
      message_t::from (std::string ("prime-b")));
    assert (second_prime.submit ().result ());

    const auto returned = std::get<authority_stored_t> (
      store->compare_exchange_authority (
        runtime::actor_authority_key (key.global_id),
        moved.store_version,
        authority_retarget_t{
          runtime::encode_actor_authority_payload (
            old_actor, "actor-send-spot", 1),
          old_placement})
        .result ().value ()).snapshot;

    // A late duplicate notice for the retired source fence cannot invalidate
    // the successor B route selected by B7. Destroy the first client before
    // dispatch so its exact unregister cannot remove the remaining client's
    // subscription.
    client.reset ();
    source->dispatch_message_follow (
      protocol::message_follow_notice_t{
        .source = source_fence,
        .target = target_fence,
        .hop_count = 1,
        .queued_messages = 1,
        .queued_bytes = 1,
        .original_operation = *request_operation,
        .original_reply_route_id = 0});

    const auto reads_before_late_call =
      store->authority_reads.load (std::memory_order_relaxed);
    actor_send_call_t later (
      *second_client,
      actor_id_t (key.global_id),
      "AfterFollowNotice",
      message_t::from (std::string ("later")));
    assert (later.submit ().result ());
    // A cached call performs only the deletion-only preflight read. A cache
    // miss would add a second authority read and select the returned A route.
    assert (store->authority_reads.load (std::memory_order_relaxed)
              - reads_before_late_call
            == 1);
    assert (collect_actor_sends (*new_target, 2s, 3) == 3);
    assert (collect_actor_sends (*old_target, 20ms) == 0);

    const auto returned_fence = protocol::actor_route_fence_t{
      key.global_id,
      old_actor.object_generation (),
      old_status.routing_id ().to_bytes (),
      old_status.lifecycle_generation (),
      returned.authority_owner_generation,
      static_cast<std::uint64_t> (old_owner.lease_generation)};
    const auto matching_notice = protocol::message_follow_notice_t{
      .source = target_fence,
      .target = returned_fence,
      .hop_count = 1,
      .queued_messages = 1,
      .queued_bytes = 1,
      .original_operation = *request_operation};
    source->dispatch_message_follow (matching_notice);
    source->dispatch_message_follow (matching_notice);
    const auto reads_before_remaining_call =
      store->authority_reads.load (std::memory_order_relaxed);
    actor_send_call_t remaining (
      *second_client,
      actor_id_t (key.global_id),
      "RemainingClientAfterNotice",
      message_t::from (std::string ("remaining")));
    assert (remaining.submit ().result ());
    assert (store->authority_reads.load (std::memory_order_relaxed)
              - reads_before_remaining_call
            == 2);
    assert (collect_actor_sends (*old_target, 2s) == 1);
    assert (collect_actor_sends (*new_target, 20ms) == 0);

    assert (old_spots.matches_actor_message_follow_source (
      old_actor, source_fence));
    assert (old_spots.cleanup_expired_actor_admissions_at (
              std::chrono::steady_clock::time_point::max ())
            != 0);
    assert (!old_spots.matches_actor_message_follow_source (
      old_actor, source_fence));
    assert (old_spots.cleanup_expired_actor_admissions_at (
              std::chrono::steady_clock::time_point::max ())
            == 0);

    source->stop ();
    second_client.reset ();
    old_target->stop ();
    new_target->stop ();
}

void verify_session_relocation_gateway_commit_is_atomic ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "game.actor",
      "relocating-actor", 7);
    const auto session_owner =
      zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session =
      zlink::routing_id_t::from (std::string ("session-rid"));
    assert (gateway.bind_session_sink (
      source,
      [] (std::string, stream_codec_t,
          const zlink::message_t &) {
          return task_t<void> (result_t<void>::success ());
      }));
    assert (gateway.record_bound_session_route (
      source, session_owner, session, 5, 11, 13, 17, 0, 19));

    const stateful::stream_binding_t previous{
      {session.to_string (), 3},
      17,
      {stateful::object_kind_t::actor, "relocating-actor", 7,
       11, "game", "source-node"},
      4,
      13};
    const stateful::stream_binding_t target{
      previous.connection,
      previous.binding_generation,
      {stateful::object_kind_t::actor, "relocating-actor", 7,
       12, "game", "target-node"},
      23,
      29};
    const protocol::session_relocation_route_t route{
      {31, 37},
      {"source-owner", 13,
       zlink::routing_id_t::from (std::string ("source-node"))
         .to_bytes (),
       4, "authority-v1"},
      protocol::relocation_role_t::target,
      {"relocating-actor", 7},
      session_owner.to_bytes (),
      5,
      "session-owner-lease",
      41,
      session.to_bytes (),
      17,
      {protocol::session_relocation_route_action_t::commit,
       11,
       12,
       zlink::routing_id_t::from (std::string ("target-node"))
         .to_bytes (),
       23,
       0}};
    const auto target_ref = actor_ref_access_t::make (
      node_rid_t::from_string ("target-node"), "game.actor",
      "relocating-actor", 7);
    assert (gateway.update_actor_ref (target_ref));
    {
        const std::lock_guard lock (state->mutex);
        const auto actor = state->actors_by_id.find (
          "relocating-actor");
        assert (actor != state->actors_by_id.end ());
        assert (actor->second.ref.node_rid ().value ()
                == "source-node");
    }
    assert (gateway.commit_session_relocation_route (
      route, previous, target));
    const auto committed_route = gateway.bound_session_route (source);
    assert (committed_route
            && committed_route->node_rid == session_owner
            && committed_route->session_rid
                 == std::make_optional (session)
            && committed_route->node_generation == 5
            && committed_route->authority_owner_generation == 12
            && committed_route->owner_lease_generation == 29
            && committed_route->binding_generation == 17
            && committed_route->session_sequence == 19);
    {
        const std::lock_guard lock (state->mutex);
        const auto actor = state->actors_by_id.find (
          "relocating-actor");
        assert (actor != state->actors_by_id.end ());
        assert (actor->second.ref.node_rid ().value ()
                == "target-node");
    }

    /* A later relocation can return the same Actor incarnation to its first
     * node. The physical Session and binding generation stay fixed while the
     * Actor authority, owner lease, and admitted high-water advance again. */
    assert (gateway.admit_session_relay (
      target_ref, session_owner, session, 17, 20));
    const stateful::stream_binding_t returned{
      target.connection,
      target.binding_generation,
      {stateful::object_kind_t::actor, "relocating-actor", 7,
       13, "game", "source-node"},
      4,
      31};
    auto return_route = route;
    return_route.relocation = {41, 43};
    return_route.actor = {"relocating-actor", 7};
    return_route.route.previous_authority_owner_generation = 12;
    return_route.route.target_authority_owner_generation = 13;
    return_route.route.target_node_routing_id =
      zlink::routing_id_t::from (std::string ("source-node"))
        .to_bytes ();
    return_route.route.target_node_generation = 4;
    assert (gateway.commit_session_relocation_route (
      return_route, target, returned));
    const auto returned_ref = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "game.actor",
      "relocating-actor", 7);
    const auto returned_route = gateway.bound_session_route (
      returned_ref);
    assert (returned_route
            && returned_route->node_rid == session_owner
            && returned_route->session_rid
                 == std::make_optional (session)
            && returned_route->node_generation == 5
            && returned_route->authority_owner_generation == 13
            && returned_route->owner_lease_generation == 31
            && returned_route->binding_generation == 17
            && returned_route->session_sequence == 20);
}

void verify_mesh_node_role_is_available_before_local_descriptor_publish ()
{
    auto state = std::make_shared<
      zlink::framework::detail::mesh_node_builder_state_t> (
        "discovery-role");
    state->object_role = zlink::framework::object_role_t::client;
    state->channels.emplace (
      "object-channel",
      zlink::framework::detail::mesh_channel_registration_t{
        .weight = 100,
        .role_selected = true,
        .server = false});

    zlink::framework::detail::mesh_node_runtime_t mesh_runtime (state);
    assert (mesh_runtime.object_role () == zlink::framework::object_role_t::client);
    assert (mesh_runtime.channel_weights ().empty ());

    zlink::framework::mesh_node_descriptor_t remote;
    remote.object_role = zlink::framework::object_role_t::client;
    assert (zlink::framework::runtime::mesh::route_mesh_connection_not_required (
      mesh_runtime.object_role (), !mesh_runtime.channel_weights ().empty (),
      remote.object_role, !remote.channel_weights.empty ()));
}

void verify_mesh_stop_drains_admitted_request_completion ()
{
    using namespace zlink::framework;

    auto source_state = std::make_shared<detail::mesh_node_builder_state_t> (
      "m6b-mesh");
    source_state->listen_endpoint = "tcp://127.0.0.1:0";
    source_state->routing_id = zlink::routing_id_t::from (
      "completion-drain-source");
    const auto core_context = std::make_shared<zlink::context_t> ();
    source_state->core_context = core_context;
    auto target_state = std::make_shared<detail::mesh_node_builder_state_t> (
      "m6b-mesh");
    target_state->listen_endpoint = "tcp://127.0.0.1:0";
    target_state->routing_id = zlink::routing_id_t::from (
      "completion-drain-target");
    target_state->core_context = core_context;

    detail::mesh_node_runtime_t source (source_state);
    detail::mesh_node_runtime_t target (target_state);
    target.start ();
    source.start ();
    const auto target_status = target.status ();
    source.connect_peer (
      target_status.routing_id (), target_status.local_endpoint (),
      target_status.lifecycle_generation ());

    const auto connected_deadline = std::chrono::steady_clock::now () + 5s;
    const auto discard = [] (const host::ready_record_t &,
                             const host::receive_record_t &,
                             std::vector<zlink::message_t>) {};
    while ((!source.has_admitted_peer (
              target_status.routing_id (),
              target_status.lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < connected_deadline) {
        (void) source.dispatch_ready (discard);
        (void) target.dispatch_ready (discard);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.has_admitted_peer (
      target_status.routing_id (), target_status.lifecycle_generation ()));

    host::call_id_t operation;
    assert (source.request_to_node (
              target_status.routing_id (),
              {zlink::message_t::from (std::string ("message-follow"))},
              operation, 2s)
              .result ()
              .value ()
            == zlink::submit_result_t::ok);

    std::optional<host::reply_token_t> delayed_reply;
    const auto admitted_deadline = std::chrono::steady_clock::now () + 2s;
    while (!delayed_reply
           && std::chrono::steady_clock::now () < admitted_deadline) {
        (void) target.dispatch_ready (
          [&] (const host::ready_record_t &,
               const host::receive_record_t &record,
               std::vector<zlink::message_t>) {
              if (record.kind == host::record_kind_t::node_request)
                  delayed_reply = record.reply_token;
          });
        std::this_thread::sleep_for (1ms);
    }
    assert (delayed_reply);

    auto completion = std::async (
      std::launch::async,
      [&] {
          return source.wait_for_completion (
            operation, 2s, target_status.routing_id ());
      });
    const auto waiter_deadline = std::chrono::steady_clock::now () + 1s;
    while ((source.active_completion_waiters () != 1
            || source.pending_transport_operations () != 1)
           && std::chrono::steady_clock::now () < waiter_deadline) {
        std::this_thread::yield ();
    }
    assert (source.active_completion_waiters () == 1);
    assert (source.pending_transport_operations () == 1);

    auto stopped = std::async (std::launch::async, [&] { source.stop (); });
    assert (stopped.wait_for (20ms) == std::future_status::timeout);
    assert (host::reply (
              *delayed_reply,
              {zlink::message_t::from (std::string ("settled"))})
            == zlink::submit_result_t::ok);

    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) source.dispatch_ready (discard);
        (void) target.dispatch_ready (discard);
        std::this_thread::sleep_for (1ms);
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    const auto settled = completion.get ();
    assert (settled);
    assert (settled.value ().record.terminal_result
            == static_cast<int> (zlink::request_result_t::ok));
    assert (settled.value ().parts.size () == 1);
    assert (settled.value ().parts.front ().to_string () == "settled");
    assert (stopped.wait_for (1s) == std::future_status::ready);
    stopped.get ();
    target.stop ();
}

void verify_remote_bound_session_bind_classifies_retryable_outcomes ()
{
    using namespace zlink::framework;

    auto state = std::make_shared<detail::mesh_node_builder_state_t> (
      "m6b-mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id =
      zlink::routing_id_t::from ("bound-session-reply-source");
    const auto core_context = std::make_shared<zlink::context_t> ();
    state->core_context = core_context;

    std::size_t invalidations = 0;
    std::size_t route_resolutions = 0;
    std::optional<protocol::actor_route_fence_t> invalidated;
    detail::mesh_node_runtime_t source (state);
    source.configure_actor_route_resolver (
      [&] (const actor_ref_t &) -> std::optional<runtime::spot_address_t> {
          ++route_resolutions;
          return std::nullopt;
      },
      [&] (const protocol::actor_route_fence_t &route) {
          ++invalidations;
          invalidated = route;
      });

    auto target_state =
      std::make_shared<detail::mesh_node_builder_state_t> (
        "m6b-mesh");
    target_state->listen_endpoint = "tcp://127.0.0.1:0";
    target_state->routing_id =
      zlink::routing_id_t::from ("bound-session-reply-target");
    target_state->core_context = core_context;
    detail::mesh_node_runtime_t target (target_state);
    target.start ();
    source.start ();
    const auto target_descriptor =
      target.native_node ().transport ().topology ().local_descriptor ();
    const auto target_rid =
      target.status ().routing_id ();
    source.connect_peer (
      target_rid, target.status ().local_endpoint (),
      target.status ().lifecycle_generation ());

    const auto peer_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (
              target_rid, target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < peer_deadline) {
        (void) source.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        (void) target.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    assert (source.has_admitted_peer (
      target_rid, target.status ().lifecycle_generation ()));
    assert (target.has_admitted_peer (
      source.status ().routing_id (),
      source.status ().lifecycle_generation ()));

    const actor_ref_t actor =
      detail::actor_ref_access_t::make (
        node_rid_t::from_string (target_rid.to_string ()),
        "bound.actor", "bound-actor", 7);
    const runtime::spot_address_t route{
      "m6b-mesh", target_rid, {}, 0, {}, 7, 11,
      {"bound-owner", 13}, target_descriptor.lifecycle_generation};

    const auto request_and_reply =
      [&] (protocol::request_terminal_result terminal,
           protocol::framework_error_code failure) {
          auto result = std::async (
            std::launch::async,
            [&] {
                return source.bind_application_actor_session (
                  actor,
                  zlink::routing_id_t::from (
                    "bound-session-reply-session"),
                  1, route, 2s)
                  .result ();
            });
          const auto deadline =
            std::chrono::steady_clock::now () + 2s;
          bool replied = false;
          while (result.wait_for (0ms)
                   != std::future_status::ready
                 && std::chrono::steady_clock::now () < deadline) {
              const auto pump =
                target.native_node ().transport ().pump_one (
                  mesh::service_liveness_registry_t::clock_t::now ())
                  .result ()
                  .value ();
              assert (pump
                      != mesh::raw_mesh_pump_result_t::protocol_error);
              if (!replied
                  && pump
                       == mesh::raw_mesh_pump_result_t::infrastructure) {
                  auto claim =
                    target.native_node ().transport ().mailbox ().try_claim (
                    mesh::service_mailbox_domain_t::infrastructure,
                    1, 4096);
                  if (claim && claim->records.size () == 1) {
                      assert (
                        protocol::decode_header (
                          claim->records.front ().parts.front ())
                          .kind
                        == protocol::command::boundSessionBind);
                      assert (target.native_node ().transport ()
                                .reply_bound_session_bind (
                                  claim->records.front (),
                                  static_cast<std::uint32_t> (terminal),
                                  static_cast<std::uint32_t> (failure)));
                      assert (target.native_node ().transport ()
                                .mailbox ().release (*claim));
                      replied = true;
                  }
              }
              (void) source.dispatch_ready (
                [] (const auto &, const auto &, auto) {});
              std::this_thread::sleep_for (1ms);
          }
          assert (replied);
          assert (result.wait_for (0ms)
                  == std::future_status::ready);
          return result.get ();
      };

    const auto stale = request_and_reply (
      protocol::request_terminal_result::conflict,
      protocol::framework_error_code::actorLocationStale);
    assert (stale);
    assert (
      stale.value ()
      == detail::application_actor_session_bind_outcome_t::stale_route);
    assert (invalidations == 1);
    assert (invalidated);
    assert (invalidated->actor_id == "bound-actor");
    assert (invalidated->object_generation == 7);
    assert (invalidated->target_node_routing_id
            == target_descriptor.node_routing_id);
    assert (invalidated->authority_owner_generation == 11);
    assert (invalidated->owner_lease_generation == 13);

    const auto actor_not_ready = request_and_reply (
      protocol::request_terminal_result::busy,
      protocol::framework_error_code::none);
    assert (actor_not_ready);
    assert (
      actor_not_ready.value ()
      == detail::application_actor_session_bind_outcome_t::
        actor_not_ready);
    assert (invalidations == 1);
    assert (route_resolutions == 0);

    const auto ordinary_conflict = request_and_reply (
      protocol::request_terminal_result::conflict,
      protocol::framework_error_code::actorAlreadyExists);
    assert (!ordinary_conflict);
    assert (ordinary_conflict.error_kind ()
            == framework_error_kind_t::unavailable);
    assert (invalidations == 1);
    assert (route_resolutions == 0);

    //  A route that never becomes available bounds its retry with the same
    //  operation_terminal_t::timed_out contract that
    //  verify_bound_session_bind_permanent_absence_is_bounded (m6a) asserts
    //  at the raw transport layer for the identical scenario (both cases
    //  resolve to the identical errno at the routing layer — confirmed
    //  empirically: submit_error_t::internal_errno() reports EHOSTUNREACH
    //  (113) for both this "unconnected-bound-session-target" case and
    //  m6a's dead-listener "bind-timeout-target" case, since neither
    //  scenario ever establishes a physical connection before the request,
    //  so a distinct ECONNREFUSED-class "connect was actively refused" path
    //  does not exist at this layer for either test to be classified
    //  against). At this higher layer bind_application_actor_session maps a
    //  timed_out terminal to deadline_exceeded (see mesh_node_runtime.cpp's
    //  bind_application_actor_session completion callback), not
    //  unavailable. This assertion previously read `unavailable`, which
    //  only held because a raw_route_port_t::request bug (fixed alongside
    //  this test change — see raw_route_port.cpp's raw_route_port_t::request
    //  try/catch scope) let this scenario's synchronous EHOSTUNREACH escape
    //  the retry classification entirely and fail immediately as
    //  transport_failed instead of exhausting the intended bounded retry.
    //  Parity note: the equivalent classification for "route never admitted
    //  within a bounded deadline" has not been cross-checked against the
    //  java/node/dotnet failure-code table (Unavailable=13 vs Deadline=19)
    //  converged elsewhere in this campaign; flagged for follow-up, not
    //  blocking here since cpp's own two-layer contract (raw timed_out +
    //  this derived kind) is now internally consistent.
    const runtime::spot_address_t disconnected_route{
      "m6b-mesh",
      zlink::routing_id_t::from ("unconnected-bound-session-target"),
      {}, 0, {}, 7, 11, {"bound-owner", 13}, 1};
    const auto transport_failure =
      source.bind_application_actor_session (
        actor,
        zlink::routing_id_t::from (
          "bound-session-reply-session"),
        1, disconnected_route, 20ms)
        .result ();
    assert (!transport_failure);
    assert (transport_failure.error_kind ()
            == framework_error_kind_t::deadline_exceeded);
    assert (invalidations == 1);
    assert (route_resolutions == 0);

    assert (detail::can_retry_application_actor_session_bind (
      detail::application_actor_session_bind_outcome_t::stale_route,
      detail::application_actor_session_bind_attempt_t::initial));
    assert (detail::can_retry_application_actor_session_bind (
      detail::application_actor_session_bind_outcome_t::actor_not_ready,
      detail::application_actor_session_bind_attempt_t::initial));
    assert (!detail::can_retry_application_actor_session_bind (
      detail::application_actor_session_bind_outcome_t::stale_route,
      detail::application_actor_session_bind_attempt_t::retry));
    assert (!detail::can_retry_application_actor_session_bind (
      detail::application_actor_session_bind_outcome_t::actor_not_ready,
      detail::application_actor_session_bind_attempt_t::retry));
    source.stop ();
    target.stop ();
}

void verify_local_session_binding_uses_location_authority ()
{
    using namespace zlink::framework;

    const auto actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("target-node"), "PlayerActor",
      "reconnected-player", 7);
    const runtime::stateful::object_ref_t materialized{
      runtime::stateful::object_kind_t::actor,
      "reconnected-player", 7, 3, "mesh", "target-node"};
    const runtime::spot_address_t route{
      "mesh", zlink::routing_id_t::from ("target-node"), "room", 11,
      "version", 7, 5, {"owner", 13}, 17};

    const auto verified =
      detail::make_local_application_actor_session_ref (
        materialized, actor, route);
    assert (verified);
    assert (verified.value ().authority_owner_generation == 5);
    assert (verified.value ().mesh_name == "mesh");
    assert (verified.value ().node_id == "target-node");

    stateful::stream_session_registry_t sessions (
      [materialized] (const std::string &) {
          return std::make_optional (materialized);
      });
    const auto connection = sessions.open ("reconnected-session");
    assert (sessions.bind (
              connection, verified.value (), route.node_generation,
              static_cast<std::uint64_t> (
                route.owner.lease_generation))
              .first
            == stateful::stateful_error_t::generation_stale);
    const auto [bind_error, binding] = sessions.bind_remote (
      connection, verified.value (), route.node_generation,
      static_cast<std::uint64_t> (route.owner.lease_generation));
    assert (bind_error == stateful::stateful_error_t::none);
    assert (binding.actor == verified.value ());
    assert (binding.actor.authority_owner_generation == 5);
    assert (binding.target_node_generation == route.node_generation);
    assert (binding.owner_lease_generation
            == static_cast<std::uint64_t> (
              route.owner.lease_generation));

    auto stale_route = route;
    stale_route.object_generation = 8;
    assert (!detail::make_local_application_actor_session_ref (
      materialized, actor, stale_route));
    auto wrong_actor = materialized;
    wrong_actor.key = "other-actor";
    assert (!detail::make_local_application_actor_session_ref (
      wrong_actor, actor, route));
}

protocol::actor_route_fence_t same_node_bound_session_route ()
{
    return protocol::actor_route_fence_t{
      "same-node-actor", 7,
      zlink::routing_id_t::from ("same-node-owner").to_bytes (),
      11, 13, 17};
}

void verify_same_node_bound_session_accepts_current_store_fence ()
{
    assert (
      host::classify_bound_session_bind_admission (
        same_node_bound_session_route (),
        host::route_fence_t{13, 17}, true)
      == host::bound_session_bind_admission_t::ready);
}

void verify_bound_session_waits_for_current_local_actor_materialization ()
{
    assert (
      host::classify_bound_session_bind_admission (
        same_node_bound_session_route (),
        host::route_fence_t{13, 17}, false)
      == host::bound_session_bind_admission_t::actor_not_ready);
}

void verify_bound_session_rejects_mismatched_store_fence ()
{
    assert (
      host::classify_bound_session_bind_admission (
        same_node_bound_session_route (),
        host::route_fence_t{14, 18}, true)
      == host::bound_session_bind_admission_t::stale_route);
    assert (
      host::classify_bound_session_bind_admission (
        same_node_bound_session_route (), std::nullopt, true)
      == host::bound_session_bind_admission_t::stale_route);
}

class memory_relocation_repository_t final :
    public stateful::relocation_store_port_t
{
  public:
    stateful::relocation_stored_t put (
      const std::vector<std::uint8_t> &payload,
      std::chrono::hours) override
    {
        std::lock_guard lock (_mutex);
        const auto reference = "instance-root-"
          + std::to_string (++_sequence);
        _roots.insert_or_assign (reference, payload);
        return {reference,
                stateful::maintenance_runtime_t::crc32c (payload)};
    }

    std::optional<std::vector<std::uint8_t>> get (
      const std::string &reference) override
    {
        std::lock_guard lock (_mutex);
        const auto found = _roots.find (reference);
        return found == _roots.end ()
          ? std::optional<std::vector<std::uint8_t>>{}
          : std::make_optional (found->second);
    }

    void remove (const std::string &reference) override
    {
        std::lock_guard lock (_mutex);
        _roots.erase (reference);
    }

    std::size_t size () const
    {
        std::lock_guard lock (_mutex);
        return _roots.size ();
    }

  private:
    mutable std::mutex _mutex;
    std::uint64_t _sequence = 0;
    std::map<std::string, std::vector<std::uint8_t>> _roots;
};

class execution_mode_spot_t final
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit execution_mode_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

class recording_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int request_submissions{0};
    std::atomic_bool delay_next_request{false};
    zlink::framework::detail::task_completion_source_t<zlink::framework::message_t>
      delayed_request;

  protected:
    zlink::framework::task_t<void> send_erased (
      zlink::framework::actor_id_t,
      std::string,
      zlink::framework::message_t,
      const zlink::framework::actor_send_call_t::metadata_map_t &) override
    {
        return zlink::framework::task_t<void> (
          zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<zlink::framework::message_t>
    request_erased (
      zlink::framework::actor_id_t,
      std::string,
      zlink::framework::message_t,
      std::optional<std::chrono::milliseconds>,
      const zlink::framework::actor_request_call_t::metadata_map_t &) override
    {
        request_submissions.fetch_add (1);
        if (delay_next_request.exchange (false))
            return delayed_request.task ();
        return zlink::framework::task_t<zlink::framework::message_t> (
          zlink::framework::result_t<zlink::framework::message_t>::success (
            zlink::framework::message_t{}));
    }

    zlink::framework::serializer_registry_t &actor_client_serializers () override
    {
        return serializers;
    }

  private:
    zlink::framework::serializer_registry_t serializers;
};

zlink::framework::task_t<zlink::framework::message_t>
request_after_actor_await (recording_actor_client_t &client)
{
    using namespace zlink::framework;
    actor_request_call_t other (
      client, actor_id_t ("other"), "OtherRequest", message_t{});
    try {
        (void) co_await other.submit_message ();
    }
    catch (const framework_exception_t &error) {
        co_return result_t<message_t>::failure (error.kind (), error.what ());
    }
    actor_request_call_t self (
      client, actor_id_t ("actor-1"), "SelfRequest", message_t{});
    try {
        co_return co_await self.submit_message ();
    }
    catch (const framework_exception_t &error) {
        co_return result_t<message_t>::failure (error.kind (), error.what ());
    }
}

void verify_spot_id_contract ()
{
    using namespace zlink::framework;
    static_assert (std::is_same_v<spot_id_t, std::string>);

    const auto user = detail::new_user_spot_id ();
    assert (user.size () == 36);
    assert (user[8] == '-' && user[13] == '-' && user[18] == '-'
            && user[23] == '-' && user[14] == '4');
    assert (user[19] == '8' || user[19] == '9'
            || user[19] == 'a' || user[19] == 'b');
    assert (detail::valid_spot_id (user));

    const auto entry = detail::new_entry_spot_id ("server");
    assert (entry.starts_with ("server-entry-"));
    assert (detail::is_framework_entry_spot_id (entry));
    assert (entry != detail::new_entry_spot_id ("server"));

    assert (!detail::valid_spot_id (""));
    assert (!detail::valid_spot_id (std::string (256, 'x')));
    assert (!detail::valid_spot_id (std::string ("\xc3\x28", 2)));
    assert (detail::valid_spot_id ("Room") && detail::valid_spot_id ("room"));

    bool rejected = false;
    try {
        (void) spot_ref_t (
          std::string (256, 'x'), 1, "mesh",
          node_rid_t::from_string ("node"));
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);

    const auto max_generation = static_cast<std::uint64_t> (
      std::numeric_limits<std::int64_t>::max ());
    const auto max_ref = spot_ref_t (
      "room", max_generation, "mesh", node_rid_t::from_string ("node"));
    assert (max_ref.object_generation () == max_generation);
    rejected = false;
    try {
        (void) spot_ref_t (
          "room", max_generation + 1, "mesh",
          node_rid_t::from_string ("node"));
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);
}

void verify_spot_route_fence_admission_precedes_body_decode ()
{
    auto state = std::make_shared<spots::spot_context_state_t> ();
    state->spot_id = "room";
    state->object_generation = 7;
    state->authority_owner_generation = 11;
    const zlink::framework::location_owner_token_t owner{"node-owner", 31};
    const protocol::spot_route_fence_t valid{
      "room", 7, {}, 3, 11, 31};

    assert (state->accepts_route_fence (valid, owner));

    auto stale = valid;
    stale.object_generation++;
    // General Spot messages address the logical Spot. Recreating the Spot on
    // the same live owner must not turn an already-routed message stale.
    assert (state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.authority_owner_generation++;
    assert (!state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.owner_lease_generation++;
    assert (!state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.spot_id = "other";
    assert (!state->accepts_route_fence (stale, owner));
    assert (!state->accepts_route_fence (valid,
                                         std::optional<zlink::framework::location_owner_token_t>{}));
}

void verify_public_host_route_cache_stops_at_owner_admission_deadline ()
{
    using namespace zlink::framework;
    auto store = std::make_shared<runtime::in_memory_location_repository_t> ();
    const auto owner = std::get<owner_lease_claimed_t> (
      store->claim_owner_lease ("route-cache-owner", 5s)
        .result ().value ()).token;
    mesh_node_descriptor_t location{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("route-cache-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities =
        {{.object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room"}},
      .object_role = object_role_t::server,
      .capacity = {.spots = {.limit = 8}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "route-cache-test",
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
    assert (store->update_mesh_node (
              location, location_write_intent_t::new_claim)
              .result ().value ().status
            == location_write_status_t::stored);
    const object_reserve_request_t reserve{
      .key = {placement_object_kind_t::user_spot, "route-cache-spot"},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = location.mesh_name,
                 .node_rid = node_rid_t::from_string (
                   location.rid.to_string ()),
                 .node_lifecycle_generation = location.lifecycle_generation,
                 .owner = owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          placement_object_kind_t::user_spot, "room", 1}}};
    const auto reserved = std::get<object_reserved_t> (
      store->reserve (reserve).result ().value ());
    const auto committed = std::get<object_committed_t> (
      store->commit ({reserve.key, reserved.fence, {std::byte{1}}})
        .result ().value ());

    host::host_options_t source_options{
      mesh::raw_mesh_node_options_t{descriptor ("route-cache-source")}};
    source_options.object_stable_types.insert ("framework.spot");
    source_options.route_cache_max_age = 10s;
    source_options.owner_lease_fencing_margin = 3s;
    auto source = std::make_shared<host::public_host_runtime_t> (
      std::move (source_options));
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        descriptor ("route-cache-target")}});
    source->configure_user_spot_operations (
      store,
      [] (const stateful::object_ref_t &, const std::string &,
          const std::vector<std::byte> &) {
          return host::user_spot_materialize_result_t{true, std::nullopt};
      });
    source->start ();
    target->start ();
    assert (source->connect_peer (
      target->status ().local_endpoint (), target->status ().routing_id ()));
    const auto noop_dispatch = [] (const host::ready_record_t &,
                                   const host::receive_record_t &,
                                   std::vector<zlink::message_t>) {};
    const auto connect_deadline = std::chrono::steady_clock::now () + 5s;
    while (!source->transport ().topology ().peer (
             target->status ().routing_id ().to_bytes ())
           && std::chrono::steady_clock::now () < connect_deadline) {
        (void) source->dispatch_ready (noop_dispatch);
        (void) target->dispatch_ready (noop_dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (
      target->status ().routing_id ().to_bytes ()));

    auto caller = source->entry_spot ();
    const auto send = [&] {
        return caller.send_to_spot (
          target->status ().routing_id (), reserve.key.global_id,
          committed.ready.object_generation,
          {zlink::message_t::from (std::string ("payload"))})
          .result ()
          .value ();
    };
    assert (send () == zlink::submit_result_t::ok);
    std::this_thread::sleep_for (3s);
    assert (send () == zlink::submit_result_t::not_found);

    source->close ();
    target->close ();
}

void verify_entry_spot_identity_claim_is_global_and_fenced ()
{
    using namespace zlink::framework;
    runtime::in_memory_location_repository_t store;
    const auto first_owner =
      std::get<owner_lease_claimed_t> (
        store.claim_owner_lease (
          "entry-owner-a", 15s)
          .result ().value ()).token;
    const auto second_owner =
      std::get<owner_lease_claimed_t> (
        store.claim_owner_lease (
          "entry-owner-b", 15s)
          .result ().value ()).token;
    const auto entry_id =
      detail::new_entry_spot_id ("entry-node");
    const auto descriptor = [&] (
      std::string rid,
      std::uint64_t lifecycle,
      const location_owner_token_t &owner,
      std::string id) {
        return mesh_node_descriptor_t{
          .mesh_name = "entry-mesh",
          .rid = zlink::routing_id_t::from (
            std::move (rid)),
          .lifecycle_generation = lifecycle,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .entry_spot_id = std::move (id),
          .application_version = 1,
          .object_capabilities =
            {{.object_kind =
                placement_object_kind_t::user_spot,
              .stable_type = "room"}},
          .object_role = object_role_t::server,
          .capacity = {.spots = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "entry-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
    };
    const auto first = descriptor (
      "entry-node-a", 41, first_owner, entry_id);
    const auto second = descriptor (
      "entry-node-b", 42, second_owner, entry_id);
    assert (
      store.update_mesh_node (
        first, location_write_intent_t::new_claim)
        .result ().value ().status
      == location_write_status_t::stored);
    runtime::store_location_resolvers_t resolver (store);
    const auto first_route =
      resolver.resolve_spot_address (
        first.mesh_name, entry_id)
        .result ().value ();
    assert (first_route);
    assert (
      first_route->node_rid.to_string ()
      == first.rid.to_string ());
    assert (
      store.update_mesh_node (
        second, location_write_intent_t::new_claim)
        .result ().value ().status
      == location_write_status_t::rejected_conflict);

    object_reserve_request_t reserved_entry{
      .key = {
        placement_object_kind_t::user_spot,
        entry_id},
      .intent = {.stable_type = "room"},
      .target =
        {.mesh_name = first.mesh_name,
         .node_rid = node_rid_t::from_string (
           first.rid.to_string ()),
         .node_lifecycle_generation =
           first.lifecycle_generation,
         .owner = first_owner},
      .capacity_bundle =
        {.spot_slots = 1,
         .spot_type =
           spot_type_capacity_delta_t{
             placement_object_kind_t::user_spot,
             "room",
             1}}};
    assert (
      std::holds_alternative<object_reserve_conflict_t> (
        store.reserve (reserved_entry)
          .result ().value ()));

    assert (
      store.remove_mesh_node (
        {first.mesh_name, first.rid}, first_owner)
        .result ().value ()
      == location_write_status_t::stored);
    assert (
      store.update_mesh_node (
        second, location_write_intent_t::new_claim)
        .result ().value ().status
      == location_write_status_t::stored);
    resolver.invalidate_spot_address (entry_id);
    const auto replacement_route =
      resolver.resolve_spot_address (
        second.mesh_name, entry_id)
        .result ().value ();
    assert (replacement_route);
    assert (
      replacement_route->node_rid.to_string ()
      == second.rid.to_string ());
    assert (
      store.remove_mesh_node (
        {first.mesh_name, first.rid}, first_owner)
        .result ().value ()
      == location_write_status_t::ignored_stale);

    const auto occupied_id =
      detail::new_user_spot_id ();
    auto occupied_target = descriptor (
      "occupied-target", 43, first_owner,
      detail::new_entry_spot_id ("occupied-target"));
    assert (
      store.update_mesh_node (
        occupied_target,
        location_write_intent_t::new_claim)
        .result ().value ().status
      == location_write_status_t::stored);
    object_reserve_request_t occupied_request{
      .key = {
        placement_object_kind_t::user_spot,
        occupied_id},
      .intent = {.stable_type = "room"},
      .target =
        {.mesh_name = occupied_target.mesh_name,
         .node_rid = node_rid_t::from_string (
           occupied_target.rid.to_string ()),
         .node_lifecycle_generation =
           occupied_target.lifecycle_generation,
         .owner = first_owner},
      .capacity_bundle =
        {.spot_slots = 1,
         .spot_type =
           spot_type_capacity_delta_t{
             placement_object_kind_t::user_spot,
             "room",
             1}}};
    assert (
      std::holds_alternative<object_reserved_t> (
        store.reserve (occupied_request)
          .result ().value ()));
    auto conflicting_descriptor = descriptor (
      "occupied-entry", 44, second_owner,
      occupied_id);
    assert (
      store.update_mesh_node (
        conflicting_descriptor,
        location_write_intent_t::new_claim)
        .result ().value ().status
      == location_write_status_t::rejected_conflict);
}

void verify_user_spot_execution_mode_registration ()
{
    using namespace zlink::framework;

    spot_node_builder_t builder;
    const auto factory = [] (spot_context_t context) {
        return std::make_shared<execution_mode_spot_t> (std::move (context));
    };
    builder.add_spot_factory<execution_mode_spot_t> (
      "wide", factory, [] (auto &configuration) {
          configuration.disable_relocation ();
      });
    builder.add_spot_factory<execution_mode_spot_t> (
      "actors", factory, [] (auto &configuration) {
          configuration.set_execution_mode (
            user_spot_execution_mode_t::per_actor);
          configuration.recreate_on_relocation ();
      });

    const auto snapshot = builder.snapshot ();
    assert (
      snapshot.spot_execution_modes.at ("wide")
      == user_spot_execution_mode_t::spot_wide);
    assert (
      snapshot.spot_execution_modes.at ("actors")
      == user_spot_execution_mode_t::per_actor);
}

void verify_self_actor_request_rejected_before_submission ()
{
    using namespace zlink::framework;

    recording_actor_client_t actor_client;
    const actor_ref_t actor =
      detail::actor_ref_access_t::make (
        node_rid_t::from_string ("node"), "player", "actor-1", 1);
    runtime::actor_execution_scope_t scope (
      "player:actor-1", "spot-1");
    actor_request_call_t async_request (
      actor_client, actor.actor_id (), "SelfRequest", message_t{});

    const auto async_result = async_request.submit_message ().result ();
    assert (!async_result);
    assert (
      async_result.error_kind ()
      == framework_error_kind_t::invalid_operation);
    actor_request_call_t yield_request (
      actor_client, actor.actor_id (), "SelfRequest", message_t{});
    const auto yield_result = yield_request.yield_message ().result ();
    assert (!yield_result);
    assert (
      yield_result.error_kind ()
      == framework_error_kind_t::invalid_operation);
    assert (actor_client.request_submissions.load () == 0);

    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (
      executor,
      runtime::serial_execution_queue_options_t{},
      {},
      runtime::serial_lane_policy_t::spot_wide ());
    std::mutex gate;
    std::condition_variable changed;
    bool yield_checked = false;
    bool release_probe = false;
    bool sibling_ran = false;
    assert (queue.try_post (
      "self-actor-yield",
      [&] {
          runtime::actor_execution_scope_t turn_scope (
            "player:actor-1", "spot-1");
          assert (runtime::actor_request_requires_current_spot_gate (
            "spot-1", false));
          assert (!runtime::actor_request_requires_current_spot_gate (
            "spot-1", true));
          assert (!runtime::actor_request_requires_current_spot_gate (
            "spot-2", false));
          actor_request_call_t request (
            actor_client, actor.actor_id (), "SelfRequest", message_t{});
          const auto result = request.yield_message ().result ();
          assert (!result);
          assert (result.error_kind ()
                  == framework_error_kind_t::invalid_operation);
          std::unique_lock lock (gate);
          yield_checked = true;
          changed.notify_all ();
          changed.wait (lock, [&] { return release_probe; });
      }));
    assert (queue.try_post ("self-actor-sibling", [&] {
        std::lock_guard lock (gate);
        sibling_ran = true;
        changed.notify_all ();
    }));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (
          lock, 1s, [&] { return yield_checked; }));
        assert (!sibling_ran);
        release_probe = true;
    }
    changed.notify_all ();
    queue.drain ();
    assert (sibling_ran);
    assert (actor_client.request_submissions.load () == 0);
}

void verify_actor_context_survives_coroutine_await ()
{
    using namespace zlink::framework;

    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor, 16);
    recording_actor_client_t actor_client;
    actor_client.delay_next_request.store (true);
    std::optional<task_t<message_t>> operation;
    queue.run ("actor-context-await", [&] {
        runtime::actor_execution_scope_t scope ("player:actor-1", "spot-1");
        operation.emplace (request_after_actor_await (actor_client));
    });
    assert (operation.has_value ());
    actor_client.delayed_request.complete (result_t<message_t>::success (message_t{}));
    const auto &result = operation->result ();
    assert (!result);
    assert (result.error_kind () == framework_error_kind_t::invalid_operation);
    assert (actor_client.request_submissions.load () == 1);
    queue.close ();
}

void verify_actor_yield_releases_spot_gate_before_reply ()
{
    using namespace zlink::framework;

    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (
      executor,
      runtime::serial_execution_queue_options_t{},
      {},
      runtime::serial_lane_policy_t::spot_wide ());
    recording_actor_client_t actor_client;
    actor_client.delay_next_request.store (true);
    std::mutex gate;
    std::condition_variable changed;
    bool sibling_ran = false;
    bool request_completed = false;
    std::shared_ptr<task_t<message_t>> request_task;

    assert (queue.try_post_async (
      "different-member-actor-yield",
      [&] (auto complete) {
          runtime::actor_execution_scope_t actor_scope (
            "player:actor-1", "spot-1");
          actor_request_call_t request (
            actor_client,
            actor_id_t ("actor-2"),
            "OtherRequest",
            message_t{});
          request_task = std::make_shared<task_t<message_t>> (
            request.yield_message ());
          observe_task_completion (
            *request_task,
            [&, request_task,
             complete = std::move (complete)] (const auto &result) mutable {
                assert (result);
                {
                    std::lock_guard lock (gate);
                    request_completed = true;
                }
                changed.notify_all ();
                complete ([] {});
            });
      }));
    assert (queue.try_post ("same-spot-sibling", [&] {
        std::lock_guard lock (gate);
        sibling_ran = true;
        changed.notify_all ();
    }));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return sibling_ran; }));
        assert (!request_completed);
    }
    assert (actor_client.request_submissions.load () == 1);
    actor_client.delayed_request.complete (
      result_t<message_t>::success (message_t{}));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (
          lock, 1s, [&] { return request_completed; }));
    }
    queue.drain ();
}

void verify_same_gate_request_rejected_before_submission ()
{
    using namespace zlink::framework;

    std::atomic_int submissions = 0;
    request_call_t<int> request (
      "SameGate",
      [&submissions] (const auto &, auto, const auto &) {
          submissions.fetch_add (1);
          return task_t<int> (result_t<int>::success (1));
      },
      [] (bool) {
          return result_t<void>::failure (
            framework_error_kind_t::invalid_operation,
            "awaited request requires the current Spot execution gate");
      });

    const auto result = request.submit ().result ();
    assert (!result);
    assert (
      result.error_kind ()
      == framework_error_kind_t::invalid_operation);
    assert (submissions.load () == 0);
}

void verify_creation_terminal_operation_isolation ()
{
    using namespace zlink::framework;
    auto store =
      std::make_shared<
        zlink::framework::runtime::in_memory_location_repository_t> ();
    const auto claimed =
      std::get<owner_lease_claimed_t> (
        store
          ->claim_owner_lease (
            "terminal-owner", std::chrono::seconds (15))
          .result ()
          .value ());
    mesh_node_descriptor_t target{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("terminal-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities =
        {{.object_kind = placement_object_kind_t::actor,
          .stable_type = "player"}},
      .object_role = object_role_t::server,
      .capacity = {.actors = {.limit = 8}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "terminal",
      .owner_id = claimed.token.owner_id,
      .lease_generation = claimed.token.lease_generation};
    assert (
      store
        ->update_mesh_node (
          target, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status == location_write_status_t::stored);
    object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "terminal-actor"},
      .intent = {.stable_type = "player"},
      .target =
        {.mesh_name = "m6b-mesh",
         .node_rid = node_rid_t::from_string ("terminal-target"),
         .node_lifecycle_generation = 1,
         .owner = claimed.token},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved =
      std::get<object_reserved_t> (
        store->reserve (request).result ().value ());
    const creation_operation_identity_t first{
      node_rid_t::from_string ("terminal-source"), 3, {5, 7}};
    const std::vector<std::byte> envelope{
      std::byte{'v'}, std::byte{'1'}};
    const creation_terminal_publication_t publication{
      first,
      envelope,
      zlink::framework::runtime::sha256 (envelope),
      std::chrono::system_clock::now ()
        + std::chrono::seconds (30)};
    assert (
      std::holds_alternative<
        object_creation_completed_result_t> (
        store
          ->complete_creation (
            {request.key, reserved.fence,
             object_creation_rejected_t{publication}})
          .result ()
          .value ()));
    assert (
      store->read_creation_terminal (first)
        .result ()
        .value ()
        .has_value ());
    const creation_operation_identity_t second{
      node_rid_t::from_string ("terminal-source"), 3, {5, 8}};
    assert (
      !store->read_creation_terminal (second)
         .result ()
         .value ()
         .has_value ());
    assert (
      std::holds_alternative<object_reserved_t> (
        store->reserve (request).result ().value ()));
}

void verify_typed_capacity_retry_uses_second_candidate ()
{
    using namespace zlink::framework;
    auto store =
      std::make_shared<
        zlink::framework::runtime::in_memory_location_repository_t> ();
    const auto first_owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("capacity-first", 15s)
          .result ().value ()).token;
    const auto second_owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("capacity-second", 15s)
          .result ().value ()).token;
    const auto publish = [&] (
      std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor{
          .mesh_name = "capacity-mesh",
          .rid = zlink::routing_id_t::from (rid),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .application_version = 1,
          .object_capabilities =
            {{.object_kind = placement_object_kind_t::user_spot,
              .stable_type = "room"}},
          .object_role = object_role_t::server,
          .capacity =
            {.spots = {.limit = 32},
             .spot_types =
               {{.object_kind =
                    placement_object_kind_t::user_spot,
                 .stable_type = "room",
                 .usage = {.limit = 1}}}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "capacity",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (
          store->update_mesh_node (
            std::move (descriptor),
            location_write_intent_t::new_claim)
            .result ().value ().status
          == location_write_status_t::stored);
    };
    publish ("capacity-node-a", first_owner);
    publish ("capacity-node-b", second_owner);
    const auto target = [] (
      std::string rid, const location_owner_token_t &owner) {
        return object_creation_target_t{
          "capacity-mesh", node_rid_t::from_string (rid), 1, owner};
    };
    object_reserve_request_t occupied{
      .key = {placement_object_kind_t::user_spot, "occupied"},
      .intent = {.stable_type = "room"},
      .target = target ("capacity-node-a", first_owner),
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .slots = 1}}};
    assert (std::holds_alternative<object_reserved_t> (
      store->reserve (occupied).result ().value ()));

    object_reserve_request_t request{
      .key = {placement_object_kind_t::user_spot, "retry-room"},
      .intent = {.stable_type = "room"},
      .target = target ("capacity-node-a", first_owner),
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .slots = 1}}};
    assert (std::holds_alternative<
      object_placement_capacity_exhausted_t> (
      store->reserve (request).result ().value ()));
    request.target = target ("capacity-node-b", second_owner);
    assert (std::holds_alternative<object_reserved_t> (
      store->reserve (request).result ().value ()));
}

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

mesh::service_node_descriptor_t descriptor (std::string rid)
{
    return {"m6b-mesh",
            bytes (std::move (rid)),
            1,
            1,
            "tcp://127.0.0.1:0",
            {},
            mesh::service_node_state_t::preparing};
}

stateful::object_ref_t create_ready (
  stateful::stateful_object_runtime_t &runtime,
  stateful::create_request_t request)
{
    const auto reserved = runtime.begin_create (request);
    assert (reserved.status == stateful::create_status_t::reserved);
    assert (reserved.factory_owner);
    assert (runtime.commit_create (reserved.attempt)
            == stateful::stateful_error_t::none);
    const auto ready = runtime.find (request.kind, request.key);
    assert (ready);
    return *ready;
}

void verify_global_identity_remote_create_and_generation_fence ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player", "room", "transient"}, 100, 16, 0, 4, 0},
       {"mesh-b", "node-b", {"player", "room", "transient"}, 100, 16, 0, 4, 0}});

    auto first = runtime.begin_create (
      {stateful::object_kind_t::actor, "actor-1", "player",
       std::string ("mesh-a"), {}, false, false});
    assert (first.status == stateful::create_status_t::reserved);
    assert (first.object.mesh_name == "mesh-a");

    const auto joined = runtime.begin_create (
      {stateful::object_kind_t::actor, "actor-1", "player",
       std::string ("mesh-b"), {}, false, false});
    assert (joined.status == stateful::create_status_t::joined);
    assert (joined.attempt == first.attempt);
    assert (!joined.factory_owner);
    const auto actor_type_mismatch = runtime.begin_create (
      {stateful::object_kind_t::actor, "actor-1", "administrator",
       std::string ("mesh-b"), {}, false, false});
    assert (actor_type_mismatch.status == stateful::create_status_t::failed);
    assert (actor_type_mismatch.error == stateful::stateful_error_t::type_mismatch);
    assert (runtime.commit_create (first.attempt)
            == stateful::stateful_error_t::none);

    const auto global_existing = runtime.begin_create (
      {stateful::object_kind_t::actor, "actor-1", "player",
       std::string ("mesh-b"), {}, false, false});
    assert (global_existing.status == stateful::create_status_t::existing);
    assert (global_existing.object.mesh_name == "mesh-a");
    assert (global_existing.object.object_generation
            == first.object.object_generation);

    const stateful::object_ref_t reserved_spot{
      stateful::object_kind_t::user_spot, "spot-1", 1, 1, "mesh-a", "node-a"};
    auto first_spot = runtime.begin_reserved_object (
      reserved_spot, "lobby", {});
    assert (first_spot.status == stateful::create_status_t::reserved);
    const auto joined_spot = runtime.begin_reserved_object (
      reserved_spot, "lobby", {});
    assert (joined_spot.status == stateful::create_status_t::joined);
    assert (joined_spot.attempt == first_spot.attempt);
    const auto spot_type_mismatch = runtime.begin_reserved_object (
      reserved_spot, "match", {});
    assert (spot_type_mismatch.status == stateful::create_status_t::failed);
    assert (spot_type_mismatch.error == stateful::stateful_error_t::type_mismatch);
    assert (runtime.commit_create (first_spot.attempt)
            == stateful::stateful_error_t::none);
    const auto existing_spot = runtime.begin_create (
      {stateful::object_kind_t::user_spot, "spot-1", "lobby",
       std::string ("mesh-b"), {}, false, false});
    assert (existing_spot.status == stateful::create_status_t::existing);
    assert (existing_spot.object.mesh_name == "mesh-a");
    assert (existing_spot.object.object_generation
            == first_spot.object.object_generation);

    const auto original = *runtime.find (
      stateful::object_kind_t::actor, "actor-1");
    assert (runtime.destroy_actor (original)
            == stateful::stateful_error_t::none);
    const auto replacement = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "actor-1", "player",
       std::string ("mesh-b"), {}, false, false});
    assert (replacement.object_generation
            == original.object_generation + 1);
    assert (runtime.destroy_actor (original)
            == stateful::stateful_error_t::generation_stale);

    // A node can retain an Actor reference while the Actor changes membership
    // on another node. When the Actor returns, the committed owner generation
    // may therefore advance by more than one from the retained local value.
    const auto returning_actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "returning-actor", "player",
       std::string ("mesh-a"), {}, false, false});
    auto remote_owner = returning_actor;
    remote_owner.authority_owner_generation++;
    remote_owner.node_id = "node-b";
    assert (runtime.adopt_reserved_actor_owner (remote_owner, "player")
            == stateful::stateful_error_t::none);
    auto returned_owner = remote_owner;
    returned_owner.authority_owner_generation += 3;
    returned_owner.node_id = "node-a";
    assert (runtime.adopt_reserved_actor_owner (returned_owner, "player")
            == stateful::stateful_error_t::none);
    auto stale_owner = returned_owner;
    stale_owner.authority_owner_generation--;
    stale_owner.node_id = "node-b";
    assert (runtime.adopt_reserved_actor_owner (stale_owner, "player")
            == stateful::stateful_error_t::generation_stale);

    // Application actors created from a globally reserved reference use the
    // same local placement capacity as actors created through begin_create.
    // Destroying that actor must release the capacity before the next
    // incarnation is reserved.
    const stateful::object_ref_t reserved_actor{
      stateful::object_kind_t::actor, "reserved-actor", 1, 1, "mesh-a", "node-a"};
    const auto reserved_create = runtime.begin_reserved_object (
      reserved_actor, "player", {});
    assert (reserved_create.status == stateful::create_status_t::reserved);
    assert (runtime.commit_create (reserved_create.attempt)
            == stateful::stateful_error_t::none);
    assert (runtime.destroy_actor (reserved_actor)
            == stateful::stateful_error_t::none);
    const auto reserved_recreated = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "reserved-actor", "player",
       std::string ("mesh-a"), {}, false, false});
    assert (reserved_recreated.object_generation
            == reserved_actor.object_generation + 1);
}

void verify_membership_turns_and_independent_infrastructure ()
{
    stateful::stateful_object_runtime_t runtime (4, 2);
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player", "room"}, 100, 16, 0, 8, 0},
       {"mesh-b", "node-b", {"player", "room"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "actor-turn", "player",
       std::string ("mesh-a"), {}, false, false});
    const auto spot = create_ready (
      runtime,
      {stateful::object_kind_t::user_spot, "spot-global", "room",
       std::string ("mesh-b"), {}, false, false});

    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application, {50, {50}})
            == stateful::stateful_error_t::none);
    assert (runtime.register_timer (actor, {7, 1000, 1000, 60})
            == stateful::stateful_error_t::none);
    const auto [prepare_error, token] =
      runtime.begin_membership_move (actor, spot);
    assert (prepare_error == stateful::stateful_error_t::none);
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application, {51, {51}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue_timer_tick (actor, 7, {60})
            == stateful::stateful_error_t::none);
    const auto [commit_error, moved_actor] =
      runtime.commit_membership_move (token);
    assert (commit_error == stateful::stateful_error_t::none);
    assert (moved_actor.object_generation == actor.object_generation);
    assert (moved_actor.authority_owner_generation
            == actor.authority_owner_generation + 1);
    assert (runtime.actor_membership (moved_actor)
            == std::optional<std::string> ("spot-global"));
    assert (runtime.timers (moved_actor)
            == std::vector<stateful::logical_timer_t> (
              {{7, 1000, 1000, 61}}));
    assert (runtime.close_spot (spot)
            == std::pair (stateful::stateful_error_t::none, false));

    for (const auto expected : {50u, 51u, 60u}) {
        const auto [queued_error, queued] = runtime.try_claim (
          moved_actor, stateful::turn_domain_t::application);
        assert (queued_error == stateful::stateful_error_t::none);
        assert (queued && queued->sequence == expected);
        assert (runtime.complete_claim (
                  moved_actor, stateful::turn_domain_t::application)
                == stateful::stateful_error_t::none);
    }

    assert (runtime.enqueue (
              moved_actor, stateful::turn_domain_t::application, {1, {1}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue (
              moved_actor, stateful::turn_domain_t::application, {2, {2}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue (
              moved_actor, stateful::turn_domain_t::infrastructure, {9, {9}})
            == stateful::stateful_error_t::none);

    const auto [first_error, first] = runtime.try_claim (
      moved_actor, stateful::turn_domain_t::application);
    assert (first_error == stateful::stateful_error_t::none);
    assert (first && first->sequence == 1);

    const auto [infra_error, infrastructure] = runtime.try_claim (
      moved_actor, stateful::turn_domain_t::infrastructure);
    assert (infra_error == stateful::stateful_error_t::none);
    assert (infrastructure && infrastructure->sequence == 9);
    assert (runtime.complete_claim (
              moved_actor, stateful::turn_domain_t::infrastructure)
            == stateful::stateful_error_t::none);

    assert (runtime.yield_claim (moved_actor, {3, {3}})
            == stateful::stateful_error_t::none);
    const auto [second_error, second] = runtime.try_claim (
      moved_actor, stateful::turn_domain_t::application);
    assert (second_error == stateful::stateful_error_t::none);
    assert (second && second->sequence == 3);
    assert (runtime.complete_claim (
              moved_actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
    const auto [continuation_error, continuation] = runtime.try_claim (
      moved_actor, stateful::turn_domain_t::application);
    assert (continuation_error == stateful::stateful_error_t::none);
    assert (continuation && continuation->sequence == 2);
    assert (runtime.complete_claim (
              moved_actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
}

void verify_instance_cold_activation_only_from_intent ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"transient"}, 100, 16, 0, 8, 0}});

    const auto forbidden = runtime.begin_create (
      {stateful::object_kind_t::instance_spot, "instance-1", "transient",
       std::nullopt, {}, false, false});
    assert (forbidden.error
            == stateful::stateful_error_t::
              instance_manager_create_forbidden);

    std::size_t factory_count = 0;
    stateful::create_request_t request{
      stateful::object_kind_t::instance_spot,
      "instance-1",
      "transient",
      std::nullopt,
      {},
      false,
      false};
    const auto activated = runtime.activate_instance (
      request, [&] (const stateful::object_ref_t &) {
          ++factory_count;
          return true;
      });
    assert (activated.status == stateful::create_status_t::reserved);
    assert (activated.error == stateful::stateful_error_t::none);
    assert (factory_count == 1);

    const auto existing = runtime.activate_instance (
      request, [&] (const stateful::object_ref_t &) {
          ++factory_count;
          return true;
      });
    assert (existing.status == stateful::create_status_t::existing);
    assert (factory_count == 1);
}

void verify_session_binding_and_terminal_once ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor,
       "session-actor",
       "player",
       std::nullopt,
       {},
       false,
       false});
    const auto second_actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor,
       "session-actor-2",
       "player",
       std::nullopt,
       {},
       false,
       false});

    std::size_t authority_reads = 0;
    stateful::stream_session_registry_t sessions (
      [&] (const std::string &actor_id) {
          ++authority_reads;
          return runtime.find (stateful::object_kind_t::actor, actor_id);
      });
    const auto connection = sessions.open ("stream-rid");
    const auto [bind_error, binding] = sessions.bind (connection, actor);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [second_bind_error, second_binding] =
      sessions.bind (connection, second_actor);
    assert (second_bind_error == stateful::stateful_error_t::none);
    assert (sessions.is_current (binding));
    assert (sessions.is_current (second_binding));
    assert (authority_reads == 2);
    const auto [dispatch_error, dispatch] =
      sessions.admit_inbound (binding);
    assert (dispatch_error == stateful::stateful_error_t::none);
    assert (dispatch && dispatch->inbound_sequence == 1);
    const auto [second_dispatch_error, second_dispatch] =
      sessions.admit_inbound (second_binding);
    assert (second_dispatch_error == stateful::stateful_error_t::none);
    assert (second_dispatch && second_dispatch->inbound_sequence == 1);
    assert (authority_reads == 2);

    assert (sessions.complete_inbound (*dispatch)
            == stateful::stateful_error_t::none);
    assert (sessions.complete_inbound (*second_dispatch)
            == stateful::stateful_error_t::none);

    const auto replacement_connection = sessions.open ("stream-rid-b");
    const auto [replacement_error, replacement] =
      sessions.bind (replacement_connection, actor);
    assert (replacement_error == stateful::stateful_error_t::none);
    assert (authority_reads == 3);
    assert (!sessions.is_current (binding));
    assert (sessions.is_current (replacement));
    assert (sessions.admit_inbound (binding).first
            == stateful::stateful_error_t::conflict);

    const auto [barrier_error, barrier] = sessions.try_seal_actor (actor);
    assert (barrier_error == stateful::stateful_error_t::none);
    auto relocated_actor = actor;
    relocated_actor.node_id = "node-relocated";
    ++relocated_actor.authority_owner_generation;
    assert (sessions.commit_barrier (barrier, relocated_actor)
            == stateful::stateful_error_t::none);
    const auto relocated_binding = sessions.current_binding (actor.key);
    assert (relocated_binding);
    assert (relocated_binding->binding_generation
            == replacement.binding_generation);
    assert (relocated_binding->actor == relocated_actor);
    const auto unchanged_binding = sessions.current_binding (second_actor.key);
    assert (unchanged_binding && unchanged_binding->actor == second_actor);
    assert (unchanged_binding->binding_generation
            == second_binding.binding_generation);
    assert (authority_reads == 3);

    assert (sessions.close (connection));
    assert (sessions.is_current (*relocated_binding));
    assert (authority_reads == 3);

    const auto reconnect = sessions.open ("stream-rid");
    assert (reconnect.connection_generation
            == connection.connection_generation + 1);
    assert (!sessions.is_current (binding));
    assert (sessions.admit_inbound (binding).first
            == stateful::stateful_error_t::conflict);
    assert (!sessions.is_current (second_binding));

    foundation::operation_registry_t operations (1);
    foundation::call_id_t id{};
    id.low = 1;
    std::atomic_size_t terminal_count{0};
    assert (operations.register_operation (
      id, foundation::operation_registry_t::clock_t::now () + 1s,
      [&] (foundation::operation_terminal_t,
           std::vector<std::uint8_t>) {
          terminal_count.fetch_add (1, std::memory_order_release);
      }));
    assert (operations.complete (id, {1}));
    assert (!operations.cancel (id));
    const auto terminal_deadline = std::chrono::steady_clock::now () + 5s;
    while (terminal_count.load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < terminal_deadline) {
        std::this_thread::yield ();
    }
    assert (terminal_count.load (std::memory_order_acquire) == 1);
}

void verify_session_ingress_sequence_is_scoped_by_actor_binding ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor_a = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-a", "player",
       std::nullopt, {}, false, false});
    const auto actor_b = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-b", "player",
       std::nullopt, {}, false, false});

    stateful::stream_session_registry_t sessions (
      [&] (const std::string &actor_id) {
          return runtime.find (stateful::object_kind_t::actor, actor_id);
      });
    const auto connection = sessions.open ("shared-stream-rid");
    const auto [bind_a_error, binding_a] = sessions.bind (
      connection, actor_a, 11, 21);
    const auto [bind_b_error, binding_b] = sessions.bind (
      connection, actor_b, 12, 22);
    assert (bind_a_error == stateful::stateful_error_t::none);
    assert (bind_b_error == stateful::stateful_error_t::none);

    const auto [admit_a_error, admitted_a] = sessions.admit_inbound (
      connection.connection_id, binding_a.binding_generation, actor_a.key, 1, 0ms);
    const auto [admit_b_error, admitted_b] = sessions.admit_inbound (
      connection.connection_id, binding_b.binding_generation, actor_b.key, 1, 0ms);
    assert (admit_a_error == stateful::stateful_error_t::none);
    assert (admit_b_error == stateful::stateful_error_t::none);
    assert (admitted_a && admitted_a->inbound_sequence == 1);
    assert (admitted_b && admitted_b->inbound_sequence == 1);
    assert (sessions.complete_inbound (*admitted_a)
            == stateful::stateful_error_t::none);
    assert (sessions.complete_inbound (*admitted_b)
            == stateful::stateful_error_t::none);

    assert (sessions.admit_inbound (
              connection.connection_id, binding_a.binding_generation,
              actor_a.key, 1, 0ms)
              .first == stateful::stateful_error_t::conflict);

    const auto sealed_a = sessions.seal_remote_route (
      connection.connection_id, binding_a.binding_generation, actor_a, 11, 21);
    assert (sealed_a.error == stateful::stateful_error_t::none);
    assert (sealed_a.last_accepted_sequence == 1);

    const auto [admit_b_second_error, admitted_b_second] =
      sessions.admit_inbound (
        connection.connection_id, binding_b.binding_generation,
        actor_b.key, 2, 0ms);
    assert (admit_b_second_error == stateful::stateful_error_t::none);
    assert (admitted_b_second && admitted_b_second->inbound_sequence == 2);
    assert (sessions.complete_inbound (*admitted_b_second)
            == stateful::stateful_error_t::none);
    const auto sealed_b = sessions.seal_remote_route (
      connection.connection_id, binding_b.binding_generation, actor_b, 12, 22);
    assert (sealed_b.error == stateful::stateful_error_t::none);
    assert (sealed_b.last_accepted_sequence == 2);
    assert (sessions.abort_barrier (sealed_b.barrier)
            == stateful::stateful_error_t::none);
    assert (sessions.abort_barrier (sealed_a.barrier)
            == stateful::stateful_error_t::none);

    const auto [rebind_error, rebound_a] = sessions.bind (
      connection, actor_a, 11, 21);
    assert (rebind_error == stateful::stateful_error_t::none);
    assert (rebound_a.binding_generation > binding_a.binding_generation);
    const auto [rebound_admit_error, rebound_admitted] =
      sessions.admit_inbound (
        connection.connection_id, rebound_a.binding_generation,
        actor_a.key, 1, 0ms);
    assert (rebound_admit_error == stateful::stateful_error_t::none);
    assert (rebound_admitted && rebound_admitted->inbound_sequence == 1);
    assert (sessions.complete_inbound (*rebound_admitted)
            == stateful::stateful_error_t::none);
}

void verify_session_route_supports_repeated_relocation ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto source = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-round-trip", "player",
       std::nullopt, {}, false, false});

    stateful::stream_session_registry_t sessions (
      [&] (const std::string &actor_id) {
          return runtime.find (stateful::object_kind_t::actor, actor_id);
      });
    const auto connection = sessions.open ("round-trip-session-rid");
    const auto [bind_error, binding] = sessions.bind (
      connection, source, 11, 21);
    assert (bind_error == stateful::stateful_error_t::none);

    const auto [first_error, first] = sessions.admit_inbound (
      connection.connection_id, binding.binding_generation, source.key, 1, 0ms);
    assert (first_error == stateful::stateful_error_t::none);
    assert (first && first->inbound_sequence == 1);
    assert (sessions.complete_inbound (*first)
            == stateful::stateful_error_t::none);

    const auto first_seal = sessions.seal_remote_route (
      connection.connection_id, binding.binding_generation, source, 11, 21);
    assert (first_seal.error == stateful::stateful_error_t::none);
    assert (first_seal.last_accepted_sequence == 1);
    auto remote = source;
    remote.node_id = "node-b";
    ++remote.authority_owner_generation;
    const auto first_commit = sessions.commit_remote_route (
      connection.connection_id, binding.binding_generation, source.key,
      source.object_generation, source.authority_owner_generation,
      remote, 12, 22);
    assert (first_commit.error == stateful::stateful_error_t::none);
    assert (first_commit.binding);
    assert (first_commit.binding->actor == remote);
    assert (!sessions.remote_route_sealed (source.key));

    const auto [second_error, second] = sessions.admit_inbound (
      connection.connection_id, binding.binding_generation, source.key, 2, 0ms);
    assert (second_error == stateful::stateful_error_t::none);
    assert (second && second->inbound_sequence == 2);
    assert (sessions.complete_inbound (*second)
            == stateful::stateful_error_t::none);

    const auto return_seal = sessions.seal_remote_route (
      connection.connection_id, binding.binding_generation, remote, 12, 22);
    assert (return_seal.error == stateful::stateful_error_t::none);
    assert (return_seal.last_accepted_sequence == 2);
    auto returned = remote;
    returned.node_id = source.node_id;
    ++returned.authority_owner_generation;
    const auto return_commit = sessions.commit_remote_route (
      connection.connection_id, binding.binding_generation, source.key,
      source.object_generation, remote.authority_owner_generation,
      returned, 11, 23);
    assert (return_commit.error == stateful::stateful_error_t::none);
    assert (return_commit.binding);
    assert (return_commit.binding->actor == returned);
    assert (return_commit.binding->connection == binding.connection);
    assert (return_commit.binding->binding_generation
            == binding.binding_generation);
    assert (!sessions.remote_route_sealed (source.key));
}

void verify_displaced_stream_binding_can_be_restored ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor,
       "restore-session-actor",
       "player",
       std::nullopt,
       {},
       false,
       false});
    stateful::stream_session_registry_t sessions (
      [&] (const std::string &actor_id) {
          return runtime.find (stateful::object_kind_t::actor, actor_id);
      });

    const auto first_connection = sessions.open ("restore-stream-a");
    const auto second_connection = sessions.open ("restore-stream-b");
    const auto [first_error, first_binding] =
      sessions.bind (first_connection, actor);
    assert (first_error == stateful::stateful_error_t::none);
    const auto [second_error, second_binding] =
      sessions.bind (second_connection, actor);
    assert (second_error == stateful::stateful_error_t::none);
    assert (sessions.current_binding (actor.key) == second_binding);

    assert (sessions.unbind (second_binding)
            == stateful::stateful_error_t::none);
    assert (sessions.restore (first_binding)
            == stateful::stateful_error_t::none);
    assert (sessions.current_binding (actor.key) == first_binding);

    const auto [third_error, third_binding] =
      sessions.bind (second_connection, actor);
    assert (third_error == stateful::stateful_error_t::none);
    assert (sessions.restore (first_binding)
            == stateful::stateful_error_t::conflict);
    assert (sessions.current_binding (actor.key) == third_binding);
}

void verify_verified_remote_stream_binding ()
{
    stateful::stream_session_registry_t sessions (
      [] (const std::string &) {
          return std::optional<stateful::object_ref_t>{};
      });
    const auto connection = sessions.open ("remote-stream");
    const stateful::object_ref_t remote_actor{
      stateful::object_kind_t::actor,
      "remote-actor", 7, 11, "mesh-a", "actor-node"};
    const auto [error, binding] = sessions.bind_remote (
      connection, remote_actor, 13, 17);
    assert (error == stateful::stateful_error_t::none);
    assert (binding.actor == remote_actor);
    assert (binding.target_node_generation == 13);
    assert (binding.owner_lease_generation == 17);
    assert (sessions.current_binding (remote_actor.key)
            == binding);

    auto invalid = remote_actor;
    invalid.authority_owner_generation = 0;
    assert (sessions.bind_remote (connection, invalid, 13, 17).first
            == stateful::stateful_error_t::invalid);
}

void verify_message_follow_route_admission_and_suppression ()
{
    using namespace zlink::framework;
    spots::actor_transfer_coordinator_t coordinator;
    const actor_ref_t target =
      detail::actor_ref_access_t::make (
        node_rid_t::from_string ("node-b"), "player", "actor-message-follow", 7);
    const spot_route_t route{
      node_rid_t::from_string ("node-b"), "spot-b", "game"};
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "actor-message-follow", 7,
      zlink::routing_id_t::from ("node-a").to_bytes (), 11, 13, 17};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "actor-message-follow", 7,
      zlink::routing_id_t::from ("node-b").to_bytes (), 12, 16, 18};
    const auto expires = std::chrono::steady_clock::now () + 30s;
    coordinator.activate_message_follow (
      "player:actor-message-follow", source_fence, target, route, target_fence,
      expires, "relocation-1");
    assert (coordinator.matches_message_follow_source (
      "player:actor-message-follow", source_fence));
    auto wrong_source_fence = source_fence;
    ++wrong_source_fence.authority_owner_generation;
    assert (!coordinator.matches_message_follow_source (
      "player:actor-message-follow", wrong_source_fence));
    const auto wrong_source = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, wrong_source_fence);
    assert (!wrong_source
            && wrong_source.error_kind ()
                 == framework_error_kind_t::unavailable);
    const auto exact_source = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, source_fence);
    assert (exact_source && exact_source.value ());
    assert (exact_source.value ()->source_fence == source_fence);
    assert (exact_source.value ()->target_fence == target_fence);
    coordinator.release_message_follow (
      "player:actor-message-follow", source_fence, 1);

    const auto missing = coordinator.try_acquire_message_follow (
      "player:missing", 7, 1, 0, source_fence);
    assert (missing && !missing.value ());
    auto newer_generation_source = source_fence;
    newer_generation_source.object_generation = 8;
    const auto stale_generation = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, newer_generation_source);
    assert (!stale_generation
            && stale_generation.error_kind ()
                 == framework_error_kind_t::invalid_operation);
    const auto hop_bound = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 8, source_fence);
    assert (!hop_bound
            && hop_bound.error_kind ()
                 == framework_error_kind_t::unavailable);
    // Message Follow admission has no relocation-specific message-count or
    // stored-size bound. Keep the acquisitions concurrent so this checks the
    // former count boundary rather than repeatedly reusing one slot.
    const auto oversized = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 16u * 1024u * 1024u + 1u, 0,
      source_fence);
    assert (oversized && oversized.value ());
    coordinator.release_message_follow (
      "player:actor-message-follow", source_fence, 16u * 1024u * 1024u + 1u);
    const auto maximum_sized = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7,
      std::numeric_limits<std::size_t>::max (), 0, source_fence);
    assert (maximum_sized && maximum_sized.value ());
    const auto accounting_overflow = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, source_fence);
    assert (!accounting_overflow
            && accounting_overflow.error_kind ()
                 == framework_error_kind_t::capacity_exceeded);
    coordinator.release_message_follow (
      "player:actor-message-follow", source_fence,
      std::numeric_limits<std::size_t>::max ());
    for (std::size_t index = 0; index != 2048; ++index) {
        const auto acquired = coordinator.try_acquire_message_follow (
          "player:actor-message-follow", 7, 1, 0, source_fence);
        assert (acquired && acquired.value ());
    }
    for (std::size_t index = 0; index != 2048; ++index)
        coordinator.release_message_follow (
          "player:actor-message-follow", source_fence, 1);

    auto repeated_source_fence = source_fence;
    repeated_source_fence.authority_owner_generation = 19;
    repeated_source_fence.owner_lease_generation = 23;
    auto repeated_target_fence = target_fence;
    repeated_target_fence.authority_owner_generation = 24;
    repeated_target_fence.owner_lease_generation = 29;
    coordinator.activate_message_follow (
      "player:actor-message-follow", repeated_source_fence, target, route,
      repeated_target_fence, expires, "relocation-2");
    const auto first_retained_source = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, source_fence);
    const auto second_retained_source = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, repeated_source_fence);
    assert (first_retained_source && first_retained_source.value ());
    assert (second_retained_source && second_retained_source.value ());
    assert (first_retained_source.value ()->target_fence == target_fence);
    assert (second_retained_source.value ()->target_fence == repeated_target_fence);
    coordinator.release_message_follow (
      "player:actor-message-follow", source_fence, 1);
    coordinator.release_message_follow (
      "player:actor-message-follow", repeated_source_fence, 1);

    std::atomic_int notification_winners{0};
    std::vector<std::thread> notification_attempts;
    for (int attempt = 0; attempt != 8; ++attempt) {
        notification_attempts.emplace_back ([&] {
            if (coordinator.try_begin_message_follow_notification (
                  "player:actor-message-follow", source_fence,
                  target_fence)) {
                notification_winners.fetch_add (
                  1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &attempt : notification_attempts)
        attempt.join ();
    assert (notification_winners.load (std::memory_order_relaxed) == 1);
    assert (!coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, target_fence));
    assert (coordinator.complete_message_follow_notification (
      "player:actor-message-follow", source_fence, target_fence, false));
    assert (coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, target_fence));
    assert (coordinator.complete_message_follow_notification (
      "player:actor-message-follow", source_fence, target_fence, true));
    assert (!coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, target_fence));
    assert (!coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, repeated_target_fence));

    auto replacement_target_fence = target_fence;
    ++replacement_target_fence.authority_owner_generation;
    ++replacement_target_fence.owner_lease_generation;
    coordinator.activate_message_follow (
      "player:actor-message-follow", source_fence, target, route,
      replacement_target_fence, expires, "relocation-1-replaced");
    assert (coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence,
      replacement_target_fence));
    assert (coordinator.complete_message_follow_notification (
      "player:actor-message-follow", source_fence,
      replacement_target_fence, true));
    assert (!coordinator.remove_message_follow (
      "player:actor-message-follow", source_fence,
      target_fence));
    assert (coordinator.matches_message_follow_source (
      "player:actor-message-follow", source_fence));
    const auto removed = coordinator.remove_message_follow (
      "player:actor-message-follow", source_fence,
      replacement_target_fence);
    assert (removed
            && removed->transfer_id
                 == "relocation-1-replaced");
    assert (!coordinator.matches_message_follow_source (
      "player:actor-message-follow", source_fence));
    assert (coordinator.matches_message_follow_source (
      "player:actor-message-follow", repeated_source_fence));
    assert (!coordinator.remove_message_follow (
      "player:actor-message-follow", source_fence,
      replacement_target_fence));

    coordinator.activate_message_follow (
      "player:expired-message-follow", source_fence, target, route, target_fence,
      std::chrono::steady_clock::now () - 1ms, "relocation-expired");
    const auto expired = coordinator.try_acquire_message_follow (
      "player:expired-message-follow", 7, 1, 0, source_fence);
    assert (!expired
            && expired.error_kind ()
                 == framework_error_kind_t::unavailable);
}

void verify_actor_commit_is_replayable_until_deadline ()
{
    using namespace zlink::framework;
    spots::actor_transfer_coordinator_t coordinator;
    const auto source = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("node-a"), "player",
      "actor-commit-replay", 7);
    detail::pending_actor_admission_t admission{
      .actor_key = "player:actor-commit-replay",
      .source_actor = source,
      .source_spot_id = "spot-a",
      .target_spot_id = "spot-b",
      .deadline = std::chrono::steady_clock::now () + 30s,
      .completion_operation_id_high = 11,
      .completion_operation_id_low = 12};
    assert (coordinator.try_add_admission ("transfer-replay", admission));
    assert (coordinator.stage_session_relocation_route (
      "transfer-replay", {0x41}, "player", 17));
    assert (coordinator.commit_session_relocation_route_authority (
      "transfer-replay", 41));
    assert (coordinator.commit_session_relocation_route_authority (
      "transfer-replay", 41));
    assert (!coordinator.commit_session_relocation_route_authority (
      "transfer-replay", 42));
    assert (coordinator.begin_commit (
      "transfer-replay", source, "spot-b"));
    coordinator.complete_commit ("transfer-replay");
    assert (!coordinator.pending_commit (
      "transfer-replay", source, "spot-b"));
    const auto completed = coordinator.completed_commit (
      "transfer-replay", source, "spot-b");
    assert (completed);
    assert (completed
              ->session_relocation_committed_authority_owner_generation
            == 41);
    assert (!coordinator.completed_commit (
      "transfer-replay", source, "different-spot"));
    assert (!coordinator.try_add_admission (
      "transfer-replay", std::move (admission)));
    assert (!coordinator.phase ("player:actor-commit-replay"));

    // Session route update is one-way. Once the target commit closes, a
    // successor move is not gated on an application-level terminal ACK.
    assert (coordinator.try_reserve_source (
      "player:actor-commit-replay", "transfer-successor"));
    assert (coordinator.phase ("player:actor-commit-replay")
            == spots::actor_move_phase_t::source_reserved);
    coordinator.cancel_move ("player:actor-commit-replay");

    auto return_admission = *completed;
    return_admission.source_spot_id = "spot-b";
    return_admission.target_spot_id = "spot-a";
    assert (coordinator.try_add_admission (
      "transfer-return", return_admission));
    assert (coordinator.phase ("player:actor-commit-replay")
            == spots::actor_move_phase_t::target_pending);
    coordinator.fail_commit ("transfer-return", false);
    assert (!coordinator.phase ("player:actor-commit-replay"));

    (void) coordinator.cleanup_expired (
      std::chrono::steady_clock::now () + 31s);
}

void verify_terminal_journal_preserves_outstanding_entries ()
{
    using journal_t = host::relocation_detail::
      bounded_terminal_journal_t<std::string, int, int>;
    using admission_kind_t = journal_t::admission_kind_t;
    const auto now = journal_t::clock_t::now ();
    journal_t journal (2, 10ms);

    assert (journal.try_begin ("outstanding", 1, now).kind
            == admission_kind_t::admitted);
    assert (journal.try_begin ("completed", 2, now).kind
            == admission_kind_t::admitted);
    assert (journal.complete ("completed", 2, 20, now));
    assert (journal.try_begin ("blocked", 3, now).kind
            == admission_kind_t::backpressured);
    assert (journal.size () == 2);
    assert (journal.try_begin ("outstanding", 1, now).kind
            == admission_kind_t::pending);
    assert (journal.try_begin ("outstanding", 9, now).kind
            == admission_kind_t::conflicting);

    assert (journal.try_begin ("after-expiry", 3, now + 11ms).kind
            == admission_kind_t::admitted);
    assert (journal.size () == 2);
    assert (journal.try_begin (
              "outstanding", 1, now + 11ms).kind
            == admission_kind_t::pending);
    assert (journal.complete (
      "after-expiry", 3, 30, now + 11ms));
    const auto replay = journal.try_begin (
      "after-expiry", 3, now + 11ms);
    assert (replay.kind == admission_kind_t::replay
            && replay.terminal == 30);
}

void verify_unbounded_actor_handoff_backlog ()
{
    using namespace zlink::framework;
    using detail::handoff_append_result_t;
    using detail::handoff_packet_t;

    const std::string actor_key = "player:actor-handoff-backlog";
    spots::actor_transfer_coordinator_t coordinator;
    assert (coordinator.try_begin_source_remote (actor_key, "transfer-1"));

    const auto packet_with_payload = [] (std::size_t bytes) {
        handoff_packet_t packet;
        packet.payload.resize (bytes);
        return packet;
    };
    //  The backlog has no record-count or stored-size bound.
    for (std::size_t index = 0; index != 2048; ++index) {
        assert (coordinator.try_append_backlog (actor_key, packet_with_payload (1))
                == handoff_append_result_t::appended);
    }
    assert (coordinator.take_backlog (actor_key).size () == 2048);

    assert (coordinator.try_append_backlog (
              actor_key, packet_with_payload (64u * 1024u * 1024u))
            == handoff_append_result_t::appended);
    assert (coordinator.try_append_backlog (
              actor_key, packet_with_payload (64u * 1024u * 1024u))
            == handoff_append_result_t::appended);
    assert (coordinator.take_backlog (actor_key).size () == 2);
    assert (coordinator.complete_move (actor_key).has_value ());

    const std::string replay_key = "player:actor-handoff-replay";
    assert (coordinator.try_begin_local (replay_key));
    assert (coordinator.try_append_backlog (replay_key, packet_with_payload (1))
            == handoff_append_result_t::appended);
    auto first_replay = coordinator.finish_move_replay (replay_key);
    assert (!first_replay.completed && first_replay.backlog.size () == 1);
    assert (coordinator.try_append_backlog (replay_key, packet_with_payload (2))
            == handoff_append_result_t::appended);
    auto late_replay = coordinator.finish_move_replay (replay_key);
    assert (!late_replay.completed && late_replay.backlog.size () == 1);
    assert (coordinator.finish_move_replay (replay_key).completed);

    const std::string reserved_key = "player:actor-handoff-reserved";
    assert (coordinator.try_reserve_source (reserved_key));
    assert (!coordinator.try_reserve_source (reserved_key));
    assert (coordinator.try_append_backlog (reserved_key, packet_with_payload (1))
            == handoff_append_result_t::appended);
    assert (coordinator.try_append_backlog (reserved_key, packet_with_payload (2))
            == handoff_append_result_t::appended);
    assert (coordinator.try_begin_source_remote (reserved_key, "transfer-reserved"));
    assert (coordinator.try_append_backlog (reserved_key, packet_with_payload (3))
            == handoff_append_result_t::appended);
    const auto reserved_backlog = coordinator.take_backlog (reserved_key);
    assert (reserved_backlog.size () == 3);
    assert (reserved_backlog[0].payload.size () == 1);
    assert (reserved_backlog[1].payload.size () == 2);
    assert (reserved_backlog[2].payload.size () == 3);
    assert (coordinator.complete_move (reserved_key).has_value ());

    const std::string cancelled_key = "player:actor-handoff-cancelled";
    assert (coordinator.try_reserve_source (cancelled_key));
    assert (coordinator.try_append_backlog (cancelled_key, packet_with_payload (1))
            == handoff_append_result_t::appended);
    coordinator.cancel_move (cancelled_key);
    assert (!coordinator.phase (cancelled_key));
    assert (coordinator.take_backlog (cancelled_key).empty ());
}

void verify_public_host_dispatches_one_application_record_per_turn ()
{
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{
          descriptor ("hwm-dispatch-source")} });
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{
          descriptor ("hwm-dispatch-target")} });
    source->start ();
    target->start ();
    const auto target_status = target->status ();
    const auto source_status = source->status ();
    assert (target->connect_peer (
      source_status.local_endpoint (), source_status.routing_id ()));

    const auto noop_dispatch = [] (const host::ready_record_t &,
                                   const host::receive_record_t &,
                                   std::vector<zlink::message_t>) {};
    const auto connect_deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while ((!source->transport ().topology ().peer (
               target_status.routing_id ().to_bytes ())
             || !target->transport ().topology ().peer (
               source_status.routing_id ().to_bytes ()))
           && mesh::service_liveness_registry_t::clock_t::now ()
                < connect_deadline) {
        (void) source->dispatch_ready (noop_dispatch);
        (void) target->dispatch_ready (noop_dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (
      target_status.routing_id ().to_bytes ()));
    assert (target->transport ().topology ().peer (
      source_status.routing_id ().to_bytes ()));

    const std::vector<zlink::message_t> first{
      zlink::message_t::from (std::string ("first"))};
    const std::vector<zlink::message_t> second{
      zlink::message_t::from (std::string ("second"))};
    assert (source->send_to_node (target_status.routing_id (), first)
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    assert (source->send_to_node (target_status.routing_id (), second)
              .result ()
              .value ()
            == zlink::submit_result_t::ok);

    const auto receive_deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (target->transport ().mailbox ().pending_messages (
             mesh::service_mailbox_domain_t::application)
             < 2
           && mesh::service_liveness_registry_t::clock_t::now ()
                < receive_deadline) {
        const auto pumped = target->transport ().pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped == mesh::raw_mesh_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (target->transport ().mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 2);

    std::size_t dispatched = 0;
    const auto dispatch = [&] (const host::ready_record_t &owner,
                               const host::receive_record_t &record,
                               std::vector<zlink::message_t> parts) {
        assert (owner.domain == host::ready_domain_t::application);
        assert (owner.owner_kind == host::owner_kind_t::node);
        assert (record.kind == host::record_kind_t::node_send);
        assert (parts.size () == 1);
        ++dispatched;
    };
    (void) target->dispatch_ready (dispatch);
    assert (dispatched == 1);
    assert (target->transport ().mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 1);

    (void) target->dispatch_ready (dispatch);
    assert (dispatched == 2);
    assert (target->transport ().mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 0);
}

void verify_local_application_enqueue_wakes_dispatch_wait ()
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{
          descriptor ("local-dispatch-wake")},
        "entry", {"framework.spot"}});
    auto entry = host->entry_spot ();
    host->start ();

    std::promise<void> waiting_started;
    auto waiting = waiting_started.get_future ();
    auto awakened = std::async (std::launch::async, [&] {
        waiting_started.set_value ();
        return host->wait_for_dispatch_activity (5s, true);
    });
    waiting.wait ();
    std::this_thread::sleep_for (20ms);
    assert (entry.publish (
              "local", "wake",
              {zlink::message_t::from (std::string ("payload"))})
            == zlink::submit_result_t::ok);
    assert (awakened.wait_for (500ms) == std::future_status::ready);
    assert (awakened.get ());

    host->close ();
}

void verify_root_location_session_seal_timeout_is_startup_snapshot ()
{
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<
      zlink::framework::runtime::in_memory_location_store_t> ();
    zlink::framework::location_options_t *configured_locations = nullptr;

    app.add_zlink_framework (
      [&] (zlink::framework::zlink_framework_options_t &options) {
          options.add_location_store (store);
          configured_locations = &options.configure_locations ();
          configured_locations->session_relocation_seal_timeout = 17ms;
      });
    assert (configured_locations != nullptr);

    configured_locations->session_relocation_seal_timeout = 29ms;
    auto provider = app.advanced ().services ().build_provider ();
    assert (
      provider
          .get_required<
            zlink::framework::runtime::location_runtime_t> ()
          .options ()
          .session_relocation_seal_timeout
      == 17ms);
}

void verify_same_node_session_seal_waits_for_active_ingress ()
{
    auto local = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{
          descriptor ("same-node-session-seal")},
        "entry", {"player"}});
    auto relocation_store =
      std::make_shared<memory_relocation_repository_t> ();
    local->configure_session_relocation_store (relocation_store);
    local->configure_session_route_owner (
      [] {
          return std::make_optional (
            zlink::framework::location_owner_token_t{
              "same-node-session-owner", 19});
      });
    local->configure_session_route_target_owner (
      [] (const std::string &actor_id,
          std::uint64_t object_generation,
          std::uint64_t authority_owner_generation,
          const zlink::routing_id_t &,
          std::uint64_t target_node_generation)
      -> std::optional<zlink::framework::location_owner_token_t> {
          if (actor_id != "same-node-session-actor"
              || object_generation == 0
              || authority_owner_generation == 0
              || target_node_generation == 0) {
              return std::nullopt;
          }
          return zlink::framework::location_owner_token_t{
            "same-node-target-owner", 23};
      });
    local->start ();
    const auto status = local->status ();
    const auto actor =
      local->create_actor ("player", "same-node-session-actor");
    const auto actor_object = local->resolve_actor (actor.ref ());
    assert (actor_object);

    const auto session_rid = bytes ("same-node-session-rid");
    const auto connection = local->sessions ().open (
      zlink::routing_id_t::from (session_rid).to_hex ());
    const auto [bind_error, binding] = local->sessions ().bind (
      connection, *actor_object, status.lifecycle_generation (), 23);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [ingress_error, ingress] =
      local->sessions ().admit_inbound (binding);
    assert (ingress_error == stateful::stateful_error_t::none);
    assert (ingress && ingress->inbound_sequence == 1);

    const protocol::session_relocation_seal_t seal{
      {81, 82},
      {"same-node-coordinator", 7, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), "store-v1"},
      protocol::relocation_role_t::coordinator,
      {actor_object->key,
       actor_object->object_generation,
       status.routing_id ().to_bytes (),
       status.lifecycle_generation (),
       actor_object->authority_owner_generation,
       23},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "same-node-session-owner",
      19,
      session_rid,
      binding.binding_generation};
    using seal_completion_t = std::pair<
      foundation::operation_terminal_t,
      std::optional<host::session_relocation_seal_result_t>>;
    std::size_t journal_capture_count = 0;
    std::promise<seal_completion_t> completion;
    auto completed = completion.get_future ();
    assert (local->seal_session_remote (
      status.routing_id (), seal, 2s,
      [&journal_capture_count] {
          ++journal_capture_count;
          return std::vector<std::uint8_t>{0x41};
      },
      [&completion] (
        foundation::operation_terminal_t terminal,
        std::optional<host::session_relocation_seal_result_t> result) {
          completion.set_value ({terminal, std::move (result)});
      })
              .result ()
              .value ());

    const auto dispatch = [] (const host::ready_record_t &,
                              const host::receive_record_t &,
                              std::vector<zlink::message_t>) {};
    (void) local->dispatch_ready (dispatch);
    assert (completed.wait_for (0ms) != std::future_status::ready);
    assert (journal_capture_count == 0);

    assert (local->sessions ().complete_inbound (*ingress)
            == stateful::stateful_error_t::none);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (completed.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        (void) local->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (completed.wait_for (0ms) == std::future_status::ready);
    const auto result = completed.get ();
    assert (result.first
            == foundation::operation_terminal_t::completed);
    assert (result.second);
    assert (journal_capture_count == 1);
    assert (relocation_store->size () == 1);

    const auto target_authority_owner_generation =
      actor_object->authority_owner_generation + 7;
    const protocol::session_relocation_route_t route{
      seal.relocation,
      seal.coordinator,
      protocol::relocation_role_t::target,
      {actor_object->key, actor_object->object_generation},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "same-node-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      {protocol::session_relocation_route_action_t::commit,
       actor_object->authority_owner_generation,
       target_authority_owner_generation,
       status.routing_id ().to_bytes (),
       status.lifecycle_generation (),
       0}};
    assert (local->route_session_remote (status.routing_id (), route)
              .result ()
              .value ());
    const auto route_deadline = std::chrono::steady_clock::now () + 2s;
    auto current = local->sessions ().current_binding (actor_object->key);
    while ((!current
            || current->actor.authority_owner_generation
                 != target_authority_owner_generation)
           && std::chrono::steady_clock::now () < route_deadline) {
        (void) local->dispatch_ready (dispatch);
        current = local->sessions ().current_binding (actor_object->key);
        std::this_thread::sleep_for (1ms);
    }
    assert (current
            && current->actor.authority_owner_generation
                 == target_authority_owner_generation);
    assert (current->target_node_generation
            == status.lifecycle_generation ());
    assert (current->owner_lease_generation == 23);
    assert (!local->sessions ().remote_route_sealed (
      actor_object->key));

    local->close ();
}

void verify_configured_session_seal_timeout_closes_actual_owner ()
{
    auto options = host::host_options_t{
      mesh::raw_mesh_node_options_t{
        descriptor ("configured-session-seal-timeout")},
      "entry", {"player"}};
    options.session_relocation_seal_timeout = 17ms;
    auto local = std::make_shared<host::public_host_runtime_t> (
      std::move (options));
    auto relocation_store =
      std::make_shared<memory_relocation_repository_t> ();
    local->configure_session_relocation_store (relocation_store);
    local->configure_session_route_owner (
      [] {
          return std::make_optional (
            zlink::framework::location_owner_token_t{
              "configured-session-owner", 19});
      });
    local->start ();

    const auto status = local->status ();
    const auto actor = local->create_actor (
      "player", "configured-session-timeout-actor");
    const auto actor_object = local->resolve_actor (actor.ref ());
    assert (actor_object);
    const auto session_rid = bytes (
      "configured-session-timeout-rid");
    const auto connection = local->sessions ().open (
      zlink::routing_id_t::from (session_rid).to_hex ());
    const auto [bind_error, binding] = local->sessions ().bind (
      connection, *actor_object,
      status.lifecycle_generation (), 23);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [ingress_error, ingress] =
      local->sessions ().admit_inbound (binding);
    assert (ingress_error == stateful::stateful_error_t::none);
    assert (ingress);

    const protocol::session_relocation_seal_t seal{
      {91, 92},
      {"configured-session-coordinator", 7,
       status.routing_id ().to_bytes (),
       status.lifecycle_generation (), "store-v1"},
      protocol::relocation_role_t::coordinator,
      {actor_object->key,
       actor_object->object_generation,
       status.routing_id ().to_bytes (),
       status.lifecycle_generation (),
       actor_object->authority_owner_generation,
       23},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "configured-session-owner",
      19,
      session_rid,
      binding.binding_generation};
    std::atomic_int journal_capture_count{0};
    std::atomic_int completion_count{0};
    std::atomic_int completion_terminal{-1};
    std::atomic_bool completion_has_result{false};
    assert (local->seal_session_remote (
      status.routing_id (), seal, 2s,
      [&journal_capture_count] {
          journal_capture_count.fetch_add (
            1, std::memory_order_acq_rel);
          return std::vector<std::uint8_t>{0x51};
      },
      [&completion_count, &completion_terminal,
       &completion_has_result] (
        foundation::operation_terminal_t terminal,
        std::optional<host::session_relocation_seal_result_t> result) {
          completion_terminal.store (
            static_cast<int> (terminal),
            std::memory_order_release);
          completion_has_result.store (
            result.has_value (), std::memory_order_release);
          completion_count.fetch_add (
            1, std::memory_order_acq_rel);
      })
              .result ()
              .value ());
    assert (completion_count.load (
              std::memory_order_acquire)
            == 0);
    assert (local->sessions ().remote_route_sealed (
      actor_object->key));

    const stateful::stream_remote_tenure_t pending_tenure{
      actor_object->key,
      actor_object->object_generation,
      actor_object->authority_owner_generation + 1,
      "configured-session-target",
      status.lifecycle_generation () + 1,
      29,
      binding.binding_generation};
    std::atomic_int held_settlement_count{0};
    std::atomic_bool held_delivered{true};
    const auto held = local->sessions ().admit_outbound (
      pending_tenure,
      stateful::stream_remote_tenure_proof_t{
        pending_tenure, "configured-session-target-owner"},
      [&held_settlement_count, &held_delivered] (bool delivered) {
          held_delivered.store (
            delivered, std::memory_order_release);
          held_settlement_count.fetch_add (
            1, std::memory_order_acq_rel);
      });
    assert (held.error == stateful::stateful_error_t::none);
    assert (held.kind
            == stateful::stream_outbound_admission_kind_t::retained);

    const auto dispatch = [] (const host::ready_record_t &,
                              const host::receive_record_t &,
                              std::vector<zlink::message_t>) {};
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (local->sessions ().current_binding (actor_object->key)
           && std::chrono::steady_clock::now () < deadline) {
        (void) local->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (!local->sessions ().current_binding (
      actor_object->key));
    assert (!local->sessions ().remote_route_sealed (
      actor_object->key));
    assert (held_settlement_count.load (
              std::memory_order_acquire)
            == 1);
    assert (!held_delivered.load (
      std::memory_order_acquire));
    assert (journal_capture_count.load (
              std::memory_order_acquire)
            == 0);
    assert (completion_count.load (
              std::memory_order_acquire)
            == 1);
    assert (completion_terminal.load (
              std::memory_order_acquire)
            == static_cast<int> (
              foundation::operation_terminal_t::timed_out));
    assert (!completion_has_result.load (
      std::memory_order_acquire));
    assert (local->sessions ().complete_inbound (*ingress)
            != stateful::stateful_error_t::none);

    const auto target_authority_owner_generation =
      actor_object->authority_owner_generation + 1;
    const protocol::session_relocation_route_t late_route{
      seal.relocation,
      seal.coordinator,
      protocol::relocation_role_t::target,
      {actor_object->key, actor_object->object_generation},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "configured-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      {protocol::session_relocation_route_action_t::commit,
       actor_object->authority_owner_generation,
       target_authority_owner_generation,
       status.routing_id ().to_bytes (),
       status.lifecycle_generation (),
       0}};
    assert (local->route_session_remote (
      status.routing_id (), late_route)
              .result ()
              .value ());
    (void) local->dispatch_ready (dispatch);
    assert (!local->sessions ().current_binding (
      actor_object->key));
    assert (held_settlement_count.load (
              std::memory_order_acquire)
            == 1);

    local->close ();
    assert (held_settlement_count.load (
              std::memory_order_acquire)
            == 1);
}

void verify_location_store_accepted_record_authority ()
{
    using namespace zlink::framework;
    runtime::in_memory_location_repository_t store;
    const auto source_owner = std::get<owner_lease_claimed_t> (
      store.claim_owner_lease ("source-owner", 15s).result ().value ()).token;
    const auto target_owner = std::get<owner_lease_claimed_t> (
      store.claim_owner_lease ("target-owner", 15s).result ().value ()).token;
    const auto register_node = [&] (std::string rid,
                                    const location_owner_token_t &owner) {
        mesh_node_descriptor_t node{
          .mesh_name = "m6b-mesh",
          .rid = zlink::routing_id_t::from (rid),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .application_version = 1,
          .object_capabilities =
            {{.object_kind = placement_object_kind_t::actor,
              .stable_type = "player"}},
          .object_role = object_role_t::server,
          .capacity = {.actors = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store.update_mesh_node (
                  std::move (node), location_write_intent_t::new_claim)
                  .result ().value ().status
                == location_write_status_t::stored);
    };
    register_node ("raw-source", source_owner);
    register_node ("raw-target", target_owner);
    const object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "actor-authority"},
      .intent = {.stable_type = "player"},
      .target = {.mesh_name = "m6b-mesh",
                 .node_rid = node_rid_t::from_string ("raw-target"),
                 .node_lifecycle_generation = 1,
                 .owner = target_owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = std::get<object_reserved_t> (
      store.reserve (request).result ().value ());
    const auto committed = std::get<object_committed_t> (
      store.commit ({request.key, reserved.fence, {std::byte{1}}})
        .result ().value ());

    const auto resolver =
      stateful::make_location_store_authority_resolver (store);
    const auto resolved = resolver ({
      .target = {stateful::object_kind_t::actor,
                 "actor-authority",
                 committed.ready.object_generation,
                 committed.ready.authority_owner_generation,
                 "m6b-mesh",
                 "raw-target"},
      .source_node_routing_id = bytes ("raw-source"),
      .source_node_generation = 1,
      .target_node_routing_id = bytes ("raw-target"),
      .target_node_generation = 1,
      .source_kind = protocol::frozen_source_kind_t::node});
    assert (resolved);
    assert (resolved->source.owner_id == source_owner.owner_id);
    assert (resolved->source.lease_generation
            == static_cast<std::uint64_t> (
                 source_owner.lease_generation));
    assert (resolved->target_owner_lease_generation
            == static_cast<std::uint64_t> (
                 target_owner.lease_generation));

    // A bound-session query carries the session triple next to the owning
    // Actor identity so resolvers can fence-check the binding.
    const auto resolved_bound = resolver ({
      .target = {stateful::object_kind_t::actor,
                 "actor-authority",
                 committed.ready.object_generation,
                 committed.ready.authority_owner_generation,
                 "m6b-mesh",
                 "raw-target"},
      .source_node_routing_id = bytes ("raw-source"),
      .source_node_generation = 1,
      .target_node_routing_id = bytes ("raw-target"),
      .target_node_generation = 1,
      .source_kind = protocol::frozen_source_kind_t::bound_session,
      .source_actor = std::make_pair (
        std::string ("actor-authority"),
        committed.ready.object_generation),
      .source_session_routing_id = bytes ("sess-1"),
      .source_binding_generation = 5,
      .source_session_sequence = 9});
    assert (resolved_bound);
    assert (resolved_bound->source.owner_id == source_owner.owner_id);
    assert (resolved_bound->target_owner_lease_generation
            == static_cast<std::uint64_t> (
                 target_owner.lease_generation));

    auto stale = stateful::accepted_record_authority_query_t{
      .target = {stateful::object_kind_t::actor,
                 "actor-authority",
                 committed.ready.object_generation,
                 committed.ready.authority_owner_generation,
                 "m6b-mesh",
                 "raw-target"},
      .source_node_routing_id = bytes ("raw-source"),
      .source_node_generation = 2,
      .target_node_routing_id = bytes ("raw-target"),
      .target_node_generation = 1,
      .source_kind = protocol::frozen_source_kind_t::node};
    assert (!resolver (stale));
}

void verify_raw_spot_and_actor_routing ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("raw-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("raw-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!source.topology ().peer (target_descriptor.node_routing_id)
            || !target.topology ().peer (source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        (void) source.pump_one (now).result ().value ();
        (void) target.pump_one (now).result ().value ();
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::spot_route_fence_t follow_source{
      "follow-spot", 1, source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation, 2, 3};
    const protocol::spot_route_fence_t follow_target{
      "follow-spot", 1, target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation, 3, 3};
    // The diagnostic is allowed to exceed 16 MiB because that value limits
    // one encoded control envelope, not the relayed payload or queue storage.
    const protocol::message_follow_notice_t follow_notice{
      follow_source, follow_target, 1, 1,
      16u * 1024u * 1024u + 1u, {1, 1}, 0};
    assert (source.send_message_follow (
              target_descriptor.node_routing_id, follow_notice)
              .result ()
              .value ());
    bool follow_received = false;
    while (!follow_received
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pumped = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &record : claim->records) {
            if (protocol::decode_header (record.parts.front ()).kind
                == protocol::command::messageFollow) {
                assert (protocol::decode_message_follow (
                          record.parts.front ())
                         == follow_notice);
                follow_received = true;
            }
        }
        assert (target.mailbox ().release (*claim));
    }
    assert (follow_received);

    auto non_increasing_follow = follow_notice;
    std::get<protocol::spot_route_fence_t> (non_increasing_follow.target)
      .authority_owner_generation =
      std::get<protocol::spot_route_fence_t> (non_increasing_follow.source)
        .authority_owner_generation;
    assert (source.send_message_follow (
              target_descriptor.node_routing_id, non_increasing_follow)
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t non_increasing_follow_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (non_increasing_follow_pump
             != mesh::raw_mesh_pump_result_t::protocol_error
           && mesh::service_liveness_registry_t::clock_t::now () < deadline)
        non_increasing_follow_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
    assert (non_increasing_follow_pump
            == mesh::raw_mesh_pump_result_t::protocol_error);

    auto stale_follow = follow_notice;
    std::get<protocol::spot_route_fence_t> (stale_follow.target)
      .target_node_generation++;
    assert (source.send_message_follow (
              target_descriptor.node_routing_id, stale_follow)
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t stale_follow_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (stale_follow_pump != mesh::raw_mesh_pump_result_t::protocol_error
           && mesh::service_liveness_registry_t::clock_t::now () < deadline)
        stale_follow_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
    assert (stale_follow_pump == mesh::raw_mesh_pump_result_t::protocol_error);

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"m6b-mesh", "raw-target", {"room", "player"},
        100, 16, 0, 8, 0}});
    const auto spot = create_ready (
      objects,
      {stateful::object_kind_t::user_spot,
       "spot-1",
       "room",
       std::nullopt,
       {},
       false,
       false});
    const auto actor = create_ready (
      objects,
      {stateful::object_kind_t::actor,
       "actor-1",
       "player",
       std::nullopt,
       {},
       false,
       false});
    bool authority_live = true;
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [&] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          assert (query.source_node_routing_id
                  == source_descriptor.node_routing_id);
          assert (query.source_node_generation
                  == source_descriptor.lifecycle_generation);
          if (!authority_live)
              return std::nullopt;
          return stateful::accepted_record_authority_t{
            {"source-owner", 17,
             source_descriptor.node_routing_id,
             source_descriptor.lifecycle_generation},
            query.target.kind == stateful::object_kind_t::actor ? 37u : 31u};
      });

    const protocol::spot_route_fence_t spot_fence{
      "spot-1",
      spot.object_generation,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      spot.authority_owner_generation,
      31};
    assert (source.send_to_spot (
      target_descriptor.node_routing_id, "source-spot",
      spot_fence, {"SpotPacket", "application/json", bytes ("spot")})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t spot_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (spot_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        spot_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (spot_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (spot_pump == mesh::raw_mesh_pump_result_t::application);
    assert (dispatch.ingest (spot)
            == stateful::stateful_error_t::none);
    const auto [spot_delivery_error, spot_delivery] =
      dispatch.try_claim (spot);
    assert (spot_delivery_error == stateful::stateful_error_t::none);
    assert (spot_delivery
            && spot_delivery->payload.payload == bytes ("spot"));
    const auto &frozen_spot = spot_delivery->frozen;
    assert (spot_delivery->turn.payload.empty ());
    assert (spot_delivery->turn.application_record.has_value ());
    assert (frozen_spot.kind
            == protocol::frozen_record_kind_t::spot_send);
    assert (frozen_spot.source.owner_id == "source-owner");
    assert (frozen_spot.source.lease_generation == 17);
    assert (frozen_spot.operation.high
            == source_descriptor.lifecycle_generation);
    assert (frozen_spot.operation.low != 0);
    assert (dispatch.complete_async (*spot_delivery).result ().value ()
            == stateful::stateful_error_t::none);

    const protocol::wire_operation_id_t relocated_one_way_operation{
      0x91, 0x92};
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
       "spot:spot-1",
       mesh::service_mailbox_domain_t::application,
       {protocol::encode_spot_message_header (
          protocol::command::spotSend, "source-spot", spot_fence,
          relocated_one_way_operation),
        protocol::encode_application_payload (
          {"SpotPacket", "application/json", bytes ("relocated")})},
       source_descriptor.node_routing_id,
       std::nullopt,
       std::nullopt,
       source_descriptor.lifecycle_generation,
       std::make_pair (relocated_one_way_operation.high,
                       relocated_one_way_operation.low)}));
    assert (dispatch.ingest (spot)
            == stateful::stateful_error_t::none);
    const auto [relocated_error, relocated_delivery] =
      dispatch.try_claim (spot);
    assert (relocated_error == stateful::stateful_error_t::none);
    assert (relocated_delivery && !relocated_delivery->request);
    assert (relocated_delivery->payload.payload == bytes ("relocated"));
    assert (dispatch.complete_async (*relocated_delivery).result ().value ()
            == stateful::stateful_error_t::none);

    const protocol::actor_route_fence_t actor_fence{
      "actor-1",
      actor.object_generation,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      actor.authority_owner_generation,
      37};
    using request_result_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    std::promise<request_result_t> promise;
    auto future = promise.get_future ();
    assert (source.request_to_actor (
      target_descriptor.node_routing_id, std::nullopt, actor_fence,
      {"ActorPacket", "application/json", bytes ("request")}, 2s,
      [&promise] (foundation::operation_terminal_t terminal,
                  std::vector<std::uint8_t> payload) {
          promise.set_value ({terminal, std::move (payload)});
      }, protocol::wire_operation_id_t{77, 88})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t actor_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (actor_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump_now =
          mesh::service_liveness_registry_t::clock_t::now ();
        const auto source_pump =
          source.pump_one (pump_now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        actor_pump = target.pump_one (pump_now).result ().value ();
        assert (actor_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_pump == mesh::raw_mesh_pump_result_t::application);
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::none);
    const auto [actor_delivery_error, actor_delivery] =
      dispatch.try_claim (actor);
    assert (actor_delivery_error == stateful::stateful_error_t::none);
    assert (actor_delivery && actor_delivery->request);
    assert (actor_delivery->payload.payload == bytes ("request"));
    const auto &frozen_actor = actor_delivery->frozen;
    assert (actor_delivery->turn.payload.empty ());
    assert (actor_delivery->turn.application_record.has_value ());
    assert (frozen_actor.kind
            == protocol::frozen_record_kind_t::actor_request);
    assert ((frozen_actor.operation
             == protocol::wire_operation_id_t{77, 88}));
    assert (frozen_actor.reply_route_id
            && *frozen_actor.reply_route_id != 88);
    assert (frozen_actor.source.owner_id == "source-owner");
    assert (dispatch.complete_async (
                       *actor_delivery,
                       protocol::application_payload_t{
                         "ActorReply", "application/json", bytes ("reply")})
              .result ().value ()
            == stateful::stateful_error_t::none);

    while (future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump = source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (future.wait_for (0ms) == std::future_status::ready);
    const auto result = future.get ();
    assert (result.first
            == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (result.second).payload
            == bytes ("reply"));

    // A bound-session actorRequest journals the frozen source with the
    // session triple and the owning Actor identity of the current binding
    // (the fenced ingest target), instead of stripping the identity down to
    // a plain node source.
    const protocol::actor_message_header_t::bound_session_source_t
      bound_session_tail{{'s', 'e', 's', 's', '-', '1'}, 5, 9};
    const protocol::wire_operation_id_t bound_operation{0xa1, 0xa2};
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
       "actor:actor-1",
       mesh::service_mailbox_domain_t::application,
       {protocol::encode_actor_message_header (
          protocol::command::actorRequest, std::nullopt, actor_fence,
          bound_operation, 91, 0, bound_session_tail),
        protocol::encode_application_payload (
          {"ActorPacket", "application/json", bytes ("bound")})},
       source_descriptor.node_routing_id,
       91,
       91,
       source_descriptor.lifecycle_generation,
       std::make_pair (bound_operation.high, bound_operation.low)}));
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::none);
    const auto [bound_delivery_error, bound_delivery] =
      dispatch.try_claim (actor);
    assert (bound_delivery_error == stateful::stateful_error_t::none);
    assert (bound_delivery && bound_delivery->request);
    const auto &frozen_bound = bound_delivery->frozen;
    assert (frozen_bound.kind
            == protocol::frozen_record_kind_t::actor_request);
    assert (frozen_bound.source_kind
            == protocol::frozen_source_kind_t::bound_session);
    assert (frozen_bound.source_session_routing_id
            && *frozen_bound.source_session_routing_id
                 == bound_session_tail.session_routing_id);
    assert (frozen_bound.source_binding_generation
            == bound_session_tail.binding_generation);
    assert (frozen_bound.source_session_sequence
            == bound_session_tail.session_sequence);
    assert (frozen_bound.source_actor
            && frozen_bound.source_actor->first == "actor-1"
            && frozen_bound.source_actor->second == actor.object_generation);
    assert (frozen_bound.reply_route_id
            && *frozen_bound.reply_route_id != 0);
    // The canonical journal bytes decode back to the same bound-session
    // identity, so replay after relocation keeps the fence intact.
    {
        assert (bound_delivery->turn.application_record.has_value ());
        const auto encoded_bound =
          protocol::encode_frozen_application_record (
            *bound_delivery->turn.application_record);
        assert (!encoded_bound.canonical_bytes.empty ());
        const auto canonical = protocol::decode_frozen_record (
          encoded_bound.canonical_bytes);
        assert (canonical.source_kind
                == protocol::frozen_source_kind_t::bound_session);
        assert (canonical.source_session_routing_id
                && *canonical.source_session_routing_id
                     == bound_session_tail.session_routing_id);
        assert (canonical.source_binding_generation
                == bound_session_tail.binding_generation);
        assert (canonical.source_session_sequence
                == bound_session_tail.session_sequence);
        assert (canonical.source_actor
                && canonical.source_actor->first == "actor-1"
                && canonical.source_actor->second
                     == actor.object_generation);
    }
    assert (dispatch.complete_async (
                       *bound_delivery,
                       protocol::application_payload_t{
                         "ActorReply", "application/json", bytes ("bound-reply")})
              .result ().value ()
            == stateful::stateful_error_t::none);

    // A bound-session-routed actorRequest that is missing its exact fence
    // (here: zero binding generation and session sequence) is pre-Captured.
    // Ingest answers with the retryable moving rejection and never freezes
    // the frame under a fallback node/actor identity.
    const protocol::wire_operation_id_t unfenced_operation{0xb1, 0xb2};
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
       "actor:actor-1",
       mesh::service_mailbox_domain_t::application,
       {protocol::encode_actor_message_header (
          protocol::command::actorRequest, std::nullopt, actor_fence,
          unfenced_operation, 92, 0),
        protocol::encode_application_payload (
          {"ActorPacket", "application/json", bytes ("unfenced")})},
       source_descriptor.node_routing_id,
       92,
       92,
       source_descriptor.lifecycle_generation,
       std::make_pair (unfenced_operation.high, unfenced_operation.low),
       mesh::service_bound_session_source_t{
         bound_session_tail.session_routing_id, 0, 0}}));
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::moving);
    {
        const auto [unfenced_error, unfenced_delivery] =
          dispatch.try_claim (actor);
        (void) unfenced_error;
        assert (!unfenced_delivery);
    }

    // A binding that requires the fence while the frame carries no
    // bound-session tail at all is the same pre-Captured condition.
    const protocol::wire_operation_id_t tailless_operation{0xb3, 0xb4};
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
       "actor:actor-1",
       mesh::service_mailbox_domain_t::application,
       {protocol::encode_actor_message_header (
          protocol::command::actorRequest, std::nullopt, actor_fence,
          tailless_operation, 93, 0),
        protocol::encode_application_payload (
          {"ActorPacket", "application/json", bytes ("tailless")})},
       source_descriptor.node_routing_id,
       93,
       93,
       source_descriptor.lifecycle_generation,
       std::make_pair (tailless_operation.high, tailless_operation.low),
       mesh::service_bound_session_source_t{
         bound_session_tail.session_routing_id,
         bound_session_tail.binding_generation,
         bound_session_tail.session_sequence}}));
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::moving);
    {
        const auto [tailless_error, tailless_delivery] =
          dispatch.try_claim (actor);
        (void) tailless_error;
        assert (!tailless_delivery);
    }

    authority_live = false;
    assert (source.send_to_actor (
      target_descriptor.node_routing_id, std::nullopt, actor_fence,
      {"ActorPacket", "application/json", bytes ("stale-owner")})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t stale_owner_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (stale_owner_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        stale_owner_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
    }
    assert (stale_owner_pump
            == mesh::raw_mesh_pump_result_t::application);
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::conflict);
    authority_live = true;

    auto stale_owner_lease_fence = actor_fence;
    ++stale_owner_lease_fence.owner_lease_generation;
    assert (source.send_to_actor (
      target_descriptor.node_routing_id, std::nullopt,
      stale_owner_lease_fence,
      {"ActorPacket", "application/json", bytes ("stale-lease")})
              .result ()
              .value ());
    stale_owner_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (stale_owner_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        stale_owner_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
    }
    assert (stale_owner_pump
            == mesh::raw_mesh_pump_result_t::application);
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::conflict);

    auto stale_fence = actor_fence;
    ++stale_fence.object_generation;
    std::promise<request_result_t> stale_promise;
    auto stale_future = stale_promise.get_future ();
    std::size_t stale_terminal_count = 0;
    assert (source.request_to_actor (
      target_descriptor.node_routing_id, std::nullopt, stale_fence,
      {"ActorPacket", "application/json", bytes ("stale")}, 2s,
      [&stale_promise, &stale_terminal_count] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) {
          ++stale_terminal_count;
          stale_promise.set_value ({terminal, std::move (payload)});
      })
              .result ()
              .value ());
    actor_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (actor_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump_now =
          mesh::service_liveness_registry_t::clock_t::now ();
        const auto source_pump =
          source.pump_one (pump_now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        actor_pump = target.pump_one (pump_now).result ().value ();
        assert (actor_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_pump == mesh::raw_mesh_pump_result_t::application);
    assert (dispatch.ingest (actor)
            == stateful::stateful_error_t::none);
    const auto [recreated_delivery_error, recreated_delivery] =
      dispatch.try_claim (actor);
    assert (recreated_delivery_error == stateful::stateful_error_t::none);
    assert (recreated_delivery && recreated_delivery->request);
    const auto &recreated_frozen = recreated_delivery->frozen;
    assert (recreated_delivery->turn.payload.empty ());
    assert (recreated_delivery->turn.application_record.has_value ());
    assert (recreated_frozen.target);
    assert (recreated_frozen.target->object_generation
            == actor.object_generation);
    assert (dispatch.complete_async (
                       *recreated_delivery,
                       protocol::application_payload_t{
                         "ActorReply", "application/json", bytes ("recreated")})
              .result ().value ()
            == stateful::stateful_error_t::none);
    while (stale_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump = source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (stale_future.wait_for (0ms)
            == std::future_status::ready);
    const auto recreated_result = stale_future.get ();
    assert (recreated_result.first
            == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (
              recreated_result.second).payload
            == bytes ("recreated"));
    assert (stale_terminal_count == 1);
    source.close ();
    target.close ();
}

void verify_relocated_source_reply_failure_keeps_terminal_record ()
{
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("relocated-reply-target")});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"m6b-mesh", "relocated-reply-target", {"actor"},
        100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      objects,
      {stateful::object_kind_t::actor, "relocated-reply-actor", "actor",
       std::nullopt, {}, false, false});
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          return stateful::accepted_record_authority_t{
            {"source-owner", 1, query.source_node_routing_id,
             query.source_node_generation},
            1};
      });
    const protocol::actor_route_fence_t actor_fence{
      "relocated-reply-actor",
      actor.object_generation,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      actor.authority_owner_generation,
      1};
    const protocol::wire_operation_id_t operation{79, 90};
    const auto unreachable_source = bytes ("unreachable-source");
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        "actor:relocated-reply-actor",
        mesh::service_mailbox_domain_t::application,
        {protocol::encode_actor_message_header (
           protocol::command::actorRequest, std::nullopt, actor_fence,
           operation, 99),
         protocol::encode_application_payload (
           {"ActorPacket", "application/json", bytes ("request")})},
        unreachable_source,
        1,
        99,
        1,
        std::make_pair (operation.high, operation.low)}));
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::none);

    const protocol::reply_relay_t relay{
      operation,
      99,
      {0x101, 0x202},
      3,
      {"coordinator", 4, bytes ("coordinator-rid"), 5, "store"},
      1,
      1,
      0,
      protocol::framework_error_code::none};
    const protocol::application_payload_t reply{
      "ActorReply", "application/json", bytes ("reply")};
    target.close ();
    assert (!dispatch.complete_relocated_source_async (
                       actor, 1, relay, reply)
              .result ().value ());

    const auto [claim_error, delivery] = dispatch.try_claim (actor);
    assert (claim_error == stateful::stateful_error_t::none && delivery);
    assert (delivery->payload.payload == bytes ("request"));
    assert (objects.complete_claim (
              actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
    target.close ();
}

void verify_atomic_raw_stateful_ingress_commit ()
{
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("atomic-ingress-target")});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects (
      1,
      128,
      1024,
      4u * 1024u * 1024u);
    objects.replace_placement_candidates (
      {{"m6b-mesh", "atomic-ingress-target", {"player"},
        100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      objects,
      {stateful::object_kind_t::actor,
       "atomic-actor",
       "player",
       std::nullopt,
       {},
       false,
       false});

    const auto source_routing_id = bytes ("atomic-ingress-source");
    constexpr std::uint64_t source_generation = 7;
    constexpr std::uint64_t owner_lease_generation = 19;
    bool authority_live = false;
    std::size_t authority_queries = 0;
    stateful::raw_stateful_dispatch_t dispatch (
      objects,
      target,
      [&] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          ++authority_queries;
          assert (query.target == actor);
          assert (query.source_node_routing_id == source_routing_id);
          assert (query.source_node_generation == source_generation);
          if (!authority_live)
              return std::nullopt;
          return stateful::accepted_record_authority_t{
            {"atomic-source-owner", 23, source_routing_id,
             source_generation},
            owner_lease_generation};
      });
    const protocol::actor_route_fence_t fence{
      actor.key,
      actor.object_generation,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      actor.authority_owner_generation,
      owner_lease_generation};
    const auto enqueue = [&] (std::uint64_t operation_low,
                              std::string payload) {
        const protocol::wire_operation_id_t operation{41, operation_low};
        assert (target.mailbox ().try_enqueue (
          mesh::service_mailbox_record_t{
            "actor:atomic-actor",
            mesh::service_mailbox_domain_t::application,
            {protocol::encode_actor_message_header (
               protocol::command::actorSend,
               std::nullopt,
               fence,
               operation),
             protocol::encode_application_payload (
               {"ActorPacket", "application/json",
                bytes (std::move (payload))})},
            source_routing_id,
            std::nullopt,
            std::nullopt,
            source_generation,
            std::make_pair (operation.high, operation.low)}));
    };
    const auto assert_no_queued_reservation = [&] {
        assert (objects.pending (
                  actor, stateful::turn_domain_t::application)
                == 0);
        assert (objects.pending_bytes (
                  actor, stateful::turn_domain_t::application)
                == 0);
    };

    // Authority rejection is outside the accepted-ingress transaction and
    // cannot allocate sequence 1 or reserve queue capacity.
    enqueue (1, "authority-rejected");
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::conflict);
    assert_no_queued_reservation ();
    authority_live = true;

    // Exercise the object enqueue rejection through the production object
    // port while no transport claim is active. The raw transaction must
    // remove its provisional pending row and retain sequence 1.
    assert (objects.enqueue (
              actor,
              stateful::turn_domain_t::application,
              stateful::turn_record_t{900, bytes ("capacity-filler"), 1})
            == stateful::stateful_error_t::none);
    const auto filled_count = objects.pending (
      actor, stateful::turn_domain_t::application);
    const auto filled_bytes = objects.pending_bytes (
      actor, stateful::turn_domain_t::application);
    enqueue (2, "capacity-rejected");
    const auto capacity_rejection = dispatch.ingest (actor);
    assert (capacity_rejection
            == stateful::stateful_error_t::backpressured);
    assert (objects.pending (
              actor, stateful::turn_domain_t::application)
            == filled_count);
    assert (objects.pending_bytes (
              actor, stateful::turn_domain_t::application)
            == filled_bytes);
    assert (dispatch.discard_pending (actor, 900)
            == stateful::stateful_error_t::not_found);
    assert (objects.discard_application (actor, 900)
            == stateful::stateful_error_t::none);
    assert_no_queued_reservation ();

    enqueue (3, "accepted-one");
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::none);
    const auto authority_queries_after_admission = authority_queries;
    authority_live = false;
    const auto [first_error, first] = dispatch.try_claim (actor);
    assert (first_error == stateful::stateful_error_t::none && first);
    assert (first->turn.sequence == 1);
    assert (first->frozen.source.owner_id == "atomic-source-owner");
    assert (first->frozen.source.lease_generation == 23);
    assert (authority_queries == authority_queries_after_admission);
    assert_no_queued_reservation ();

    // The running item retains the sole object reservation until terminal
    // completion even though it has left the pending FIFO.
    assert (objects.enqueue (
              actor,
              stateful::turn_domain_t::application,
              stateful::turn_record_t{901, bytes ("while-running"), 1})
            == stateful::stateful_error_t::backpressured);
    assert_no_queued_reservation ();
    assert (dispatch.complete_async (*first).result ().value ()
            == stateful::stateful_error_t::none);
    assert (dispatch.complete_async (*first).result ().value ()
            == stateful::stateful_error_t::conflict);
    authority_live = true;

    enqueue (4, "accepted-two");
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::none);
    const auto [second_error, second] = dispatch.try_claim (actor);
    assert (second_error == stateful::stateful_error_t::none && second);
    assert (second->turn.sequence == 2);
    assert (dispatch.complete_async (*second).result ().value ()
            == stateful::stateful_error_t::none);

    enqueue (6, "accepted-three");
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::none);
    const auto [third_error, third] = dispatch.try_claim (actor);
    assert (third_error == stateful::stateful_error_t::none && third);
    assert (third->turn.sequence == 3);
    assert (dispatch.complete_async (*third).result ().value ()
            == stateful::stateful_error_t::none);
    assert (authority_queries == 5);
    target.close ();
}

void verify_node_request_requires_remote_admission ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("request-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("request-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    target.expect_peer (source_descriptor);
    assert (source.connect_peer (target.endpoint (), target_descriptor));

    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    // Transport readiness is not service admission. A Node direct request is
    // rejected until the remote descriptor has completed service admission.
    assert (!source.request_to_node (
      target_descriptor.node_routing_id,
      {"DeferredRequest", "application/json", bytes ("request")}, 2s,
      [] (foundation::operation_terminal_t,
          std::vector<std::uint8_t>) {})
              .result ()
              .value ());

    while ((!source.topology ().peer (target_descriptor.node_routing_id)
             || !target.topology ().peer (source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        const auto source_pump =
          source.pump_one (now).result ().value ();
        const auto target_pump =
          target.pump_one (now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    using request_result_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    std::promise<request_result_t> promise;
    auto future = promise.get_future ();
    assert (source.request_to_node (
      target_descriptor.node_routing_id,
      {"DeferredRequest", "application/json", bytes ("request")},
      2s,
      [&promise] (foundation::operation_terminal_t terminal,
                  std::vector<std::uint8_t> payload) {
          promise.set_value ({terminal, std::move (payload)});
      },
      4242)
              .result ()
              .value ());

    std::optional<mesh::service_mailbox_claim_t> claim;
    while (!claim
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        const auto source_pump =
          source.pump_one (now).result ().value ();
        const auto target_pump =
          target.pump_one (now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application, 1, 1024);
        if (!claim)
            std::this_thread::sleep_for (1ms);
    }
    assert (claim && claim->records.size () == 1);
    const auto &request = claim->records.front ();
    assert (protocol::decode_header (request.parts.front ()).kind
            == protocol::command::nodeRequest);
    assert (request.correlation && *request.correlation == 4242);
    assert (protocol::decode_application_payload (request.parts.at (1)).payload
            == bytes ("request"));
    assert (target.reply (
      request,
      {"DeferredReply", "application/json", bytes ("reply")}));
    assert (target.mailbox ().release (*claim));

    while (future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto pumped = source.pump_one (now).result ().value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (future.wait_for (0ms) == std::future_status::ready);
    const auto result = future.get ();
    assert (result.first == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (result.second).payload
            == bytes ("reply"));
    source.close ();
    target.close ();
}

void verify_unadmitted_request_is_rejected_without_framework_queue ()
{
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("overflow-target")});
    target.start ();

    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from ("overflow-source"));
    source.connect (target.endpoint ());
    std::this_thread::sleep_for (50ms);

    using native_reply_t = std::vector<zlink::message_t>;
    const auto payload = protocol::encode_application_payload (
      {"DeferredRequest", "application/json", bytes ("request")});
    auto header = zlink::message_t::from (
      protocol::encode_node_request_header (1));
    auto body = zlink::message_t::from (payload);
    auto request = std::move (
      source.request ().message (header).message (body))
                     .timeout (5s)
                     .async ();

    mesh::raw_mesh_pump_result_t pumped =
      mesh::raw_mesh_pump_result_t::no_data;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (pumped == mesh::raw_mesh_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        (void) target.drain_monitor_events (
          mesh::service_liveness_registry_t::clock_t::now ());
        pumped = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::protocol_error);
    assert (target.mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::application) == 0);

    auto rejected_reply =
      await_native_reply (std::move (request)).result ().value ();
    assert (rejected_reply.size () == 1);
    const auto reply = protocol::decode_reply_header (
      rejected_reply.front ().to_bytes ());
    assert (reply.correlation == 1);
    assert (reply.terminal_result
            == static_cast<std::uint32_t> (
              protocol::request_terminal_result::notConnected));
    assert (reply.failure_code
            == static_cast<std::uint32_t> (
              protocol::framework_error_code::none));
    target.close ();
}

void verify_full_owner_rejects_request_without_blocking_other_owner ()
{
    auto target_descriptor = descriptor ("owner-capacity-target");
    target_descriptor.channels.push_back ({"independent-channel", 100});
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("owner-capacity-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{
        .descriptor = target_descriptor,
        .application_message_budget = 1});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto published_target = target.topology ().local_descriptor ();
    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), published_target));
    while ((!source.topology ().peer (published_target.node_routing_id)
             || !target.topology ().peer (source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        assert (source.pump_one (now).result ().value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target.pump_one (now).result ().value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (published_target.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::application_payload_t payload{
      "OwnerCapacity", "application/json", bytes ("request")};
    assert (source.send_to_node (published_target.node_routing_id, payload)
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t first =
      mesh::raw_mesh_pump_result_t::no_data;
    while (first != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        first = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (first != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (first == mesh::raw_mesh_pump_result_t::application);

    std::promise<std::pair<foundation::operation_terminal_t,
                           std::vector<std::uint8_t>>> rejected_promise;
    auto rejected = rejected_promise.get_future ();
    assert (source.request_to_node (
      published_target.node_routing_id, payload, 5s,
      [&rejected_promise] (foundation::operation_terminal_t terminal,
                           std::vector<std::uint8_t> failure) {
          rejected_promise.set_value (
            {terminal, std::move (failure)});
      })
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t overflow =
      mesh::raw_mesh_pump_result_t::no_data;
    while (overflow != mesh::raw_mesh_pump_result_t::backpressured
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        overflow = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (overflow != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (overflow == mesh::raw_mesh_pump_result_t::backpressured);
    while (rejected.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        assert (source.pump_one (
                  mesh::service_liveness_registry_t::clock_t::now ())
                  .result ()
                  .value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (rejected.wait_for (0ms) == std::future_status::ready);
    auto [rejected_terminal, rejected_failure] = rejected.get ();
    assert (rejected_terminal
            == foundation::operation_terminal_t::transport_failed);
    const auto rejected_reply =
      protocol::decode_reply_header (rejected_failure);
    assert (rejected_reply.terminal_result
            == static_cast<std::uint32_t> (
              protocol::request_terminal_result::rejected));
    assert (rejected_reply.failure_code
            == static_cast<std::uint32_t> (
              protocol::framework_error_code::workerQueueFull));

    std::promise<foundation::operation_terminal_t> independent_promise;
    auto independent = independent_promise.get_future ();
    assert (source.request_to_channel (
      "independent-channel", payload, 5s,
      [&independent_promise] (foundation::operation_terminal_t terminal,
                              std::vector<std::uint8_t>) {
          independent_promise.set_value (terminal);
      })
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t independent_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (independent_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        independent_pump = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (independent_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (independent_pump == mesh::raw_mesh_pump_result_t::application);
    auto claim = target.mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      "channel:independent-channel", 1, 1024);
    assert (claim && claim->records.size () == 1);
    assert (target.reply (
      claim->records.front (),
      {"OwnerCapacityReply", "application/json", bytes ("reply")}));
    assert (target.mailbox ().release (*claim));
    while (independent.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        assert (source.pump_one (
                  mesh::service_liveness_registry_t::clock_t::now ())
                  .result ()
                  .value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (independent.wait_for (0ms) == std::future_status::ready);
    assert (independent.get ()
            == foundation::operation_terminal_t::completed);
    source.close ();
    target.close ();
}

void verify_raw_terminal_reply_relay ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("relay-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("relay-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!source.topology ().peer (target_descriptor.node_routing_id)
            || !target.topology ().peer (source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));

    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 7, target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation, "store-3"};
    const protocol::reply_relay_t relay{
      {1, 2}, 3, {4, 5}, 6, coordinator, 8, 9, 0,
      protocol::framework_error_code::none};
    const protocol::application_payload_t payload{
      "ReplyPacket", "application/json", bytes ("relay")};
    assert (target.send_reply_relay (
              source_descriptor.node_routing_id, relay, payload)
              .result ()
              .value ());

    bool received = false;
    while (!received
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pumped = source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = source.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &record : claim->records) {
            if (protocol::decode_header (record.parts.front ()).kind
                != protocol::command::replyRelay)
                continue;
            assert (protocol::decode_reply_relay (record.parts.front ())
                    == relay);
            assert (protocol::decode_application_payload (record.parts.back ())
                    == payload);
            received = true;
        }
        assert (source.mailbox ().release (*claim));
    }
    assert (received);
    source.close ();
    target.close ();
}

void verify_relocation_data_duplicates_are_distinct_arrivals ()
{
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("duplicate-data-target")});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto source_routing_id = bytes ("duplicate-data-source");
    constexpr std::uint64_t source_generation = 7;
    const protocol::relocation_id_t relocation{0x91, 0x92};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 11, bytes ("coordinator-node"), 13, "store-v1"};
    const protocol::relocation_object_t wire_object{
      protocol::relocation_object_kind_t::actor,
      {}, "duplicate-data-actor", 3, 5};
    auto registered_object = wire_object;
    registered_object.stable_type = "player";

    protocol::frozen_application_record_t accepted;
    accepted.kind = protocol::frozen_record_kind_t::actor_send;
    accepted.source_kind = protocol::frozen_source_kind_t::node;
    accepted.source = {
      "source-owner", 17, source_routing_id, source_generation};
    accepted.operation = {19, 23};
    accepted.body = protocol::frozen_actor_application_body_t{
      {wire_object.object_id,
       wire_object.object_generation,
       target_descriptor.node_routing_id,
       target_descriptor.lifecycle_generation,
       wire_object.expected_authority_owner_generation,
       29},
      {"ActorPacket", "application/json", bytes ("duplicate")}};
    const auto frozen =
      protocol::encode_frozen_application_record (accepted);
    const protocol::relocation_data_t data{
      relocation, 31, coordinator, protocol::relocation_role_t::source,
      wire_object, frozen};
    const auto encoded_data = protocol::encode_relocation_control (data);
    const mesh::service_mailbox_record_t wire_record{
      "duplicate-data-target",
      mesh::service_mailbox_domain_t::infrastructure,
      {encoded_data},
      source_routing_id,
      std::nullopt,
      std::nullopt,
      source_generation};

    std::vector<protocol::wire_operation_id_t> target_queue;
    stateful::raw_relocation_replay_coordinator_t replay (target);
    assert (replay.register_target ({
      relocation, 31, coordinator, source_routing_id, source_generation,
      registered_object,
      [&] (const protocol::relocation_data_t &arrival) {
          target_queue.push_back (arrival.record.operation);
          return true;
      },
      [&] (const protocol::relocation_data_t &) {
          assert (false && "accepted duplicate arrival rolled back");
      }}));

    assert (await_task (replay.process (wire_record))
            == stateful::raw_relocation_replay_result_t::applied);
    assert (await_task (replay.process (wire_record))
            == stateful::raw_relocation_replay_result_t::applied);
    assert ((target_queue
             == std::vector<protocol::wire_operation_id_t>{
               frozen.operation, frozen.operation}));
    target.close ();
}

void verify_relocation_ingress_boundary_owns_saved_and_follow_only_work ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"relocation-boundary", "source", {"player"},
        100, 16, 0, 16, 0}});
    const auto actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "boundary-actor", "player",
       std::string ("relocation-boundary"), {}, false, false});
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application,
              {1, bytes ("saved")}) == stateful::stateful_error_t::none);

    const auto [seal_error, seal] = await_task (
      runtime.try_seal_relocation_aggregate ({actor}));
    assert (seal_error == stateful::stateful_error_t::none);
    assert (seal.participants.size () == 1);
    assert (seal.participants.front ().pending_application.size () == 1);
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application,
              {2, bytes ("boundary")}) == stateful::stateful_error_t::none);
    const auto [boundary_error, boundary] =
      runtime.begin_relocation_boundary (seal.token);
    assert (boundary_error == stateful::stateful_error_t::none);
    assert (boundary.participants.size () == 1);
    assert (boundary.participants.front ().records.size () == 1);
    assert (boundary.participants.front ().records.front ().sequence == 2);
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application,
              {3, bytes ("later")}) == stateful::stateful_error_t::none);

    // Before Cutover, exact abort restores each source-owned segment once:
    // saved prefix, transferred boundary batch, then later held ingress.
    assert (runtime.abort_relocation_before_cutover (seal.token)
            == stateful::stateful_error_t::none);
    for (const auto expected : {1ull, 2ull, 3ull}) {
        const auto [claim_error, record] = runtime.try_claim (
          actor, stateful::turn_domain_t::application);
        assert (claim_error == stateful::stateful_error_t::none);
        assert (record && record->sequence == expected);
        assert (runtime.complete_claim (
                  actor, stateful::turn_domain_t::application)
                == stateful::stateful_error_t::none);
    }

    const auto [second_error, second] = await_task (
      runtime.try_seal_relocation_aggregate ({actor}));
    assert (second_error == stateful::stateful_error_t::none);
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application,
              {4, bytes ("post-capture")})
            == stateful::stateful_error_t::none);
    const auto [second_boundary_error, second_boundary] =
      runtime.begin_relocation_boundary (second.token);
    assert (second_boundary_error == stateful::stateful_error_t::none);
    assert (second_boundary.participants.front ().records.size () == 1);
    assert (runtime.enqueue (
              actor, stateful::turn_domain_t::application,
              {5, bytes ("follow-only")})
            == stateful::stateful_error_t::none);
    assert (runtime.finalize_relocation_cutover (second.token)
            == stateful::stateful_error_t::none);
    assert (runtime.abort_relocation_before_cutover (second.token)
            == stateful::stateful_error_t::conflict);
    const auto [claim_error, record] = runtime.try_claim (
      actor, stateful::turn_domain_t::application);
    assert (claim_error == stateful::stateful_error_t::moving);
    assert (!record);
}

void verify_durable_reply_relay_single_winner ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("terminal-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("terminal-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    auto deadline = std::chrono::steady_clock::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!source.topology ().peer (target_descriptor.node_routing_id)
            || !target.topology ().peer (source_descriptor.node_routing_id))
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::relocation_id_t relocation{301, 302};
    const protocol::wire_operation_id_t operation{401, 402};
    const protocol::relocation_coordinator_fence_t coordinator{
      "terminal-coordinator", 9, target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation, "store-terminal"};
    const protocol::request_source_fence_t request_source{
      "request-owner", 10, source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation};
    const protocol::reply_relay_t relay{
      operation, 501, relocation, 6, coordinator, 7, 8, 0,
      protocol::framework_error_code::none};
    const protocol::application_payload_t reply{
      "TerminalReply", "application/json", bytes ("completed")};

    stateful::raw_relocation_replay_coordinator_t source_coordinator (
      source, 8, 4096, 1ms, 1ms);
    stateful::raw_relocation_replay_coordinator_t target_coordinator (
      target, 8, 4096, 1ms, 1ms);
    int completion_count = 0;
    assert (source_coordinator.register_terminal_source ({
      relocation, coordinator, operation, request_source,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      6, 7, 8, 501,
      [&] (const protocol::reply_relay_t &received,
           const std::optional<protocol::application_payload_t> &payload) {
          ++completion_count;
          return received == relay && payload && *payload == reply;
      }}));
    int persisted_ack_count = 0;
    protocol::reply_relay_ack_status_t persisted_status{};
    assert (target_coordinator.register_terminal_target ({
      relay, request_source, reply,
      [&] (protocol::reply_relay_ack_status_t status) {
          ++persisted_ack_count;
          persisted_status = status;
          return true;
      },
      [] { return true; }}));
    assert (target_coordinator.pending_terminal_relays () == 1);
    assert (target_coordinator.terminal_retained_bytes () > 0);
    const protocol::reply_relay_ack_t stale_ack{
      relocation, coordinator, operation, relay.reply_route_id,
      request_source,
      protocol::reply_relay_ack_status_t::terminal_received};
    mesh::service_mailbox_record_t stale_ack_record{
      "target", mesh::service_mailbox_domain_t::infrastructure,
      {protocol::encode_reply_relay_ack (stale_ack)},
      source_descriptor.node_routing_id, std::nullopt, std::nullopt,
      source_descriptor.lifecycle_generation + 1};
    assert (await_task (target_coordinator.process (stale_ack_record))
            == stateful::raw_relocation_replay_result_t::stale_fence);
    auto wrong_route_ack = stale_ack;
    ++wrong_route_ack.reply_route_id;
    mesh::service_mailbox_record_t wrong_route_ack_record{
      "target", mesh::service_mailbox_domain_t::infrastructure,
      {protocol::encode_reply_relay_ack (wrong_route_ack)},
      source_descriptor.node_routing_id, std::nullopt, std::nullopt,
      source_descriptor.lifecycle_generation};
    assert (await_task (target_coordinator.process (wrong_route_ack_record))
            == stateful::raw_relocation_replay_result_t::stale_fence);

    const auto receive = [&] (
      mesh::raw_mesh_node_owner_t &node,
      stateful::raw_relocation_replay_coordinator_t &coordinator) {
        const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
        while (std::chrono::steady_clock::now () < receive_deadline) {
            const auto pumped = node.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ())
              .result ()
              .value ();
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
            const auto result = await_task (coordinator.pump_one ());
            if (result != stateful::raw_relocation_replay_result_t::no_data)
                return result;
        }
        return stateful::raw_relocation_replay_result_t::no_data;
    };

    const auto first_send_time =
      stateful::raw_relocation_replay_coordinator_t::clock_t::now ();
    assert (await_task (
              target_coordinator.retry_terminal_relays (first_send_time))
            == 1);
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::terminal_received);
    assert (completion_count == 1);

    bool first_ack_dropped = false;
    deadline = std::chrono::steady_clock::now () + 2s;
    while (!first_ack_dropped && std::chrono::steady_clock::now () < deadline) {
        const auto pumped = target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ())
          .result ()
          .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &record : claim->records) {
            if (protocol::decode_header (record.parts.front ()).kind
                  == protocol::command::replyRelayAck) {
                assert (protocol::decode_reply_relay_ack (
                          record.parts.front ()).status
                        == protocol::reply_relay_ack_status_t::terminal_received);
                first_ack_dropped = true;
            }
        }
        assert (target.mailbox ().release (*claim));
    }
    assert (first_ack_dropped);
    assert (target_coordinator.pending_terminal_relays () == 1);

    auto conflicting_reply = reply;
    conflicting_reply.payload = bytes ("conflicting");
    assert (target.send_reply_relay (
              source_descriptor.node_routing_id, relay, conflicting_reply)
              .result ()
              .value ());
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::conflicting_duplicate);
    assert (completion_count == 1);

    assert (await_task (target_coordinator.retry_terminal_relays (
              first_send_time + 2ms))
            == 1);
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::terminal_duplicate);
    assert (completion_count == 1);
    assert (receive (target, target_coordinator)
            == stateful::raw_relocation_replay_result_t::relay_acknowledged);
    assert (persisted_ack_count == 1);
    assert (persisted_status
            == protocol::reply_relay_ack_status_t::already_terminal);
    assert (target_coordinator.pending_terminal_relays () == 0);
    assert (target_coordinator.terminal_retained_bytes () == 0);

    auto expiry_relay = relay;
    expiry_relay.operation = {403, 404};
    expiry_relay.reply_route_id = 502;
    int expiry_persisted = 0;
    assert (target_coordinator.register_terminal_target ({
      expiry_relay, request_source, std::nullopt,
      [] (protocol::reply_relay_ack_status_t) { return true; },
      [&] {
          ++expiry_persisted;
          return true;
      }}));
    auto wrong_source = request_source;
    ++wrong_source.lease_generation;
    assert (!target_coordinator.confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, wrong_source));
    assert (target_coordinator.pending_terminal_relays () == 1);
    assert (target_coordinator.confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, request_source));
    assert (expiry_persisted == 1);
    assert (target_coordinator.pending_terminal_relays () == 0);
    assert (target_coordinator.terminal_retained_bytes () == 0);

    assert (source_coordinator.reap_terminal_tombstones (
              stateful::raw_relocation_replay_coordinator_t::clock_t::now ()
                + 2ms)
            == 1);
    assert (target.send_reply_relay (
              source_descriptor.node_routing_id, relay, reply)
              .result ()
              .value ());
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::not_registered);

    stateful::raw_relocation_replay_coordinator_t bounded_target (
      target, 1, 1, 1ms, 1ms);
    assert (!bounded_target.register_terminal_target ({
      relay, request_source, reply,
      [] (protocol::reply_relay_ack_status_t) { return true; },
      [] { return true; }}));
    auto non_ok = relay;
    non_ok.terminal_result = 101;
    non_ok.failure_code = protocol::framework_error_code::requestFailed;
    assert (!target_coordinator.register_terminal_target ({
      non_ok, request_source, reply,
      [] (protocol::reply_relay_ack_status_t) { return true; },
      [] { return true; }}));
    source.close ();
    target.close ();
}

void verify_public_host_dispatches_durable_reply_relay ()
{
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{descriptor ("host-terminal-source")}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{descriptor ("host-terminal-target")}});
    source->start ();
    target->start ();
    const auto source_status = source->status ();
    const auto target_status = target->status ();
    assert (source->connect_peer (
      target->transport ().endpoint (), target_status.routing_id ()));

    const protocol::relocation_id_t relocation{601, 602};
    const protocol::wire_operation_id_t operation{603, 604};
    const protocol::relocation_coordinator_fence_t coordinator{
      "host-coordinator", 11, target_status.routing_id ().to_bytes (),
      target_status.lifecycle_generation (), "host-store"};
    const protocol::request_source_fence_t request_source{
      "host-source-owner", 12, source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation ()};
    const protocol::reply_relay_t relay{
      operation, 605, relocation, 13, coordinator, 14, 15, 0,
      protocol::framework_error_code::none};
    int completions = 0;
    int acknowledgements = 0;
    assert (source->relocation_wire ().register_terminal_source ({
      relocation, coordinator, operation, request_source,
      target_status.routing_id ().to_bytes (),
      target_status.lifecycle_generation (), 13, 14, 15, 605,
      [&] (const protocol::reply_relay_t &received,
           const std::optional<protocol::application_payload_t> &payload) {
          ++completions;
          return received == relay && !payload;
      }}));
    assert (target->relocation_wire ().register_terminal_target ({
      relay, request_source, std::nullopt,
      [&] (protocol::reply_relay_ack_status_t) {
          ++acknowledgements;
          return true;
      },
      [] { return true; }}));
    const auto dispatch = [] (const host::ready_record_t &,
                              const host::receive_record_t &,
                              std::vector<zlink::message_t>) {};
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while ((completions != 1 || acknowledgements != 1)
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        (void) target->dispatch_ready (dispatch);
    }
    assert (completions == 1);
    assert (acknowledgements == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    auto duplicate_relay = relay;
    duplicate_relay.operation = {606, 607};
    duplicate_relay.reply_route_id = 608;
    int duplicate_completions = 0;
    int duplicate_acknowledgements = 0;
    assert (source->relocation_wire ().register_terminal_source ({
      relocation, coordinator, duplicate_relay.operation, request_source,
      target_status.routing_id ().to_bytes (),
      target_status.lifecycle_generation (), 13, 14, 15, 608,
      [&] (const protocol::reply_relay_t &received,
           const std::optional<protocol::application_payload_t> &payload) {
          ++duplicate_completions;
          return received == duplicate_relay && !payload;
      }}));
    assert (target->relocation_wire ().register_terminal_target ({
      duplicate_relay, request_source, std::nullopt,
      [&] (protocol::reply_relay_ack_status_t) {
          ++duplicate_acknowledgements;
          return true;
      },
      [] { return true; }}));

    const auto first =
      stateful::raw_relocation_replay_coordinator_t::clock_t::now ();
    assert (await_task (
              target->relocation_wire ().retry_terminal_relays (first))
            == 1);
    while (await_task (source->transport ().pump_one (
             mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (source->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::terminal_received);
    assert (duplicate_completions == 1);

    bool dropped = false;
    while (!dropped) {
        (void) await_task (target->transport ().pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        auto claim = target->transport ().mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 64 * 1024);
        if (!claim)
            continue;
        dropped = protocol::decode_header (
          claim->records.front ().parts.front ()).kind
          == protocol::command::replyRelayAck;
        assert (target->transport ().mailbox ().release (*claim));
    }
    assert (target->relocation_wire ().pending_terminal_relays () == 1);
    assert (await_task (target->relocation_wire ().retry_terminal_relays (
              first + 2s)) == 1);
    while (await_task (source->transport ().pump_one (
             mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (source->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::terminal_duplicate);
    assert (duplicate_completions == 1);
    while (await_task (target->transport ().pump_one (
             mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (target->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::relay_acknowledged);
    assert (duplicate_acknowledgements == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    auto expiry_relay = relay;
    expiry_relay.operation = {609, 610};
    expiry_relay.reply_route_id = 611;
    int lease_expiry_persisted = 0;
    assert (target->relocation_wire ().register_terminal_target ({
      expiry_relay, request_source, std::nullopt,
      [] (protocol::reply_relay_ack_status_t) { return true; },
      [&] {
          ++lease_expiry_persisted;
          return true;
      }}));
    assert (target->relocation_wire ().confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, request_source));
    assert (lease_expiry_persisted == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    source->close ();
    target->close ();
}

void verify_remote_user_spot_create_close_terminal_once ()
{
    using namespace zlink::framework;
    auto store =
      std::make_shared<zlink::framework::runtime::
                         in_memory_location_repository_t> ();
    const auto claimed =
      store->claim_owner_lease ("target-owner", 30s)
        .result ()
        .value ();
    const auto *owner =
      std::get_if<owner_lease_claimed_t> (&claimed);
    assert (owner);
    mesh_node_descriptor_t target_location{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("user-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities =
        {{.object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .policy = maintenance_policy_kind_t::recreate},
         {.object_kind = placement_object_kind_t::instance_spot,
          .stable_type = "quest",
          .policy = maintenance_policy_kind_t::recreate}},
      .object_role = object_role_t::server,
      .capacity = {
        .spots = {.limit = 100},
        .spot_types =
          {{.object_kind = placement_object_kind_t::instance_spot,
            .stable_type = "quest",
            .usage = {.limit = 100}}}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner->token.owner_id,
      .lease_generation = owner->token.lease_generation};
    assert (
      store
        ->update_mesh_node (
          target_location, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status == location_write_status_t::stored);

    const std::string spot_id = "remote-room";
    const std::vector<std::byte> creation_payload{
      std::byte{0x41}, std::byte{0x42}};
    const object_reserve_request_t reserve{
      .key = {placement_object_kind_t::user_spot,
              spot_id},
      .intent =
        {.stable_type = "room",
         .request_content_reference =
           "inline-v1:bd9444ea:QUI",
         .request_sha256 =
           zlink::framework::runtime::sha256 (creation_payload),
         .request_encoded_size = 2},
      .target =
        {.mesh_name = "m6b-mesh",
         .node_rid = node_rid_t::from_string ("user-target"),
         .node_lifecycle_generation = 1,
         .owner = owner->token},
      .creating_payload = {std::byte{0x7f}},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .slots = 1}}};
    const auto reserved =
      store->reserve (reserve).result ().value ();
    const auto *reservation =
      std::get_if<object_reserved_t> (&reserved);
    assert (reservation);
    assert (reservation->creating.pending_creation);
    assert (
      reservation->creating.pending_creation->reservation_id
      == reservation->fence.reservation_id);
    assert (
      reservation->creating.pending_creation
        ->request_content_reference
      == reserve.intent.request_content_reference);
    const std::string invalid_spot_id = "remote-room-invalid";
    auto invalid_reserve = reserve;
    invalid_reserve.key.global_id = invalid_spot_id;
    invalid_reserve.intent.request_encoded_size = 3;
    const auto invalid_reserved =
      store->reserve (invalid_reserve).result ().value ();
    const auto *invalid_reservation =
      std::get_if<object_reserved_t> (&invalid_reserved);
    assert (invalid_reservation);
    const std::string cleanup_spot_id = "remote-room-cleanup";
    auto cleanup_reserve = reserve;
    cleanup_reserve.key.global_id = cleanup_spot_id;
    const auto cleanup_reserved =
      store->reserve (cleanup_reserve).result ().value ();
    const auto *cleanup_reservation =
      std::get_if<object_reserved_t> (&cleanup_reserved);
    assert (cleanup_reservation);
    assert (
      zlink::framework::runtime::
        cleanup_source_created_reservation (
          store, cleanup_reserve.key,
          cleanup_reservation->fence, false)
      == zlink::framework::runtime::
           source_creation_cleanup_t::not_owned);
    auto wrong_cleanup_fence = cleanup_reservation->fence;
    wrong_cleanup_fence.reservation_id += "-other";
    assert (
      zlink::framework::runtime::
        cleanup_source_created_reservation (
          store, cleanup_reserve.key,
          wrong_cleanup_fence, true)
      == zlink::framework::runtime::
           source_creation_cleanup_t::stale);
    assert (
      zlink::framework::runtime::
        cleanup_source_created_reservation (
          store, cleanup_reserve.key,
          cleanup_reservation->fence, true)
      == zlink::framework::runtime::
           source_creation_cleanup_t::aborted);
    assert (
      std::holds_alternative<authority_missing_t> (
        store
          ->read_authority (
            zlink::framework::runtime::spot_authority_key (
              cleanup_spot_id))
          .result ()
          .value ()));

    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        mesh::raw_mesh_node_options_t{descriptor ("user-source")}});
    host::host_options_t target_options{
      mesh::raw_mesh_node_options_t{descriptor ("user-target")}};
    target_options.user_spot_operation_capacity = 1;
    target_options.user_spot_operation_replay_retention = 50ms;
    auto target = std::make_shared<host::public_host_runtime_t> (
      std::move (target_options));
    std::size_t materialize_count = 0;
    target->configure_user_spot_operations (
      store,
      [&materialize_count] (
        const stateful::object_ref_t &object,
        const std::string &stable_type,
        const std::vector<std::byte> &creation) {
          ++materialize_count;
          assert (object.object_generation != 0);
          assert (stable_type == "room");
          assert (creation
                  == std::vector<std::byte> (
                    {std::byte{0x41}, std::byte{0x42}}));
          return host::user_spot_materialize_result_t{
            true, std::nullopt};
      });
    std::size_t instance_activation_count = 0;
    std::size_t instance_prepare_count = 0;
    bool fail_recovery_dispatch_once = false;
    auto instance_relocations =
      std::make_shared<memory_relocation_repository_t> ();
    target->configure_instance_spot_operations (
      store, instance_relocations, owner->token,
      host::instance_spot_activation_materializer_t{
        [&instance_prepare_count] (
          const protocol::instance_spot_activation_header_t &activation) {
            ++instance_prepare_count;
            assert (!activation.target.spot_id.empty ());
            return true;
        },
        [&instance_activation_count, &store,
         &fail_recovery_dispatch_once] (
          const protocol::instance_spot_activation_header_t &activation,
          const std::optional<std::vector<std::uint8_t>> &metadata,
          const protocol::application_payload_t &application) {
            ++instance_activation_count;
            assert (activation.target.stable_type == "quest");
            assert (activation.request);
            assert (std::holds_alternative<authority_snapshot_t> (
              store->read_authority (
                zlink::framework::runtime::spot_authority_key (
                  activation.target.spot_id))
                .result ().value ()));
            assert (metadata
                    == std::optional<std::vector<std::uint8_t>> (
                      {{1, 1, 5, 't', 'r', 'a', 'c', 'e', 0, 3,
                        'a', 'b', 'c'}}));
            assert (application.packet_name == "quest.start");
            if (activation.target.spot_id == "instance-recover"
                && fail_recovery_dispatch_once) {
                fail_recovery_dispatch_once = false;
                throw std::runtime_error (
                  "simulated process failure after Ready publication");
            }
            return host::instance_spot_activation_result_t{
              0, 0,
              protocol::application_payload_t{
                "quest.reply", "application/json", {'{', '}'}}};
        }});
    source->start ();
    target->start ();
    assert (source->connect_peer (
      target->status ().local_endpoint (),
      target->status ().routing_id ()));
    auto deadline =
      std::chrono::steady_clock::now () + 5s;
    auto dispatch = [] (const host::ready_record_t &,
                        const host::receive_record_t &,
                        std::vector<zlink::message_t>) {};
    while ((!source->transport ().topology ().peer (
              target->status ().routing_id ().to_bytes ())
            || !target->transport ().topology ().peer (
              source->status ().routing_id ().to_bytes ()))
           && std::chrono::steady_clock::now () < deadline) {
        (void) source->dispatch_ready (dispatch);
        (void) target->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (
      target->status ().routing_id ().to_bytes ()));

    std::optional<protocol::reply_header_t> instance_reply_header;
    std::optional<protocol::application_payload_t> instance_reply_payload;
    protocol::instance_spot_activation_header_t instance_request{
      {target->status ().routing_id ().to_bytes (),
       target->status ().lifecycle_generation (),
       "instance-1", "m6b-mesh", "quest", "descriptor-1",
       static_cast<std::uint64_t> (
         std::chrono::duration_cast<std::chrono::milliseconds> (
           std::chrono::system_clock::now ().time_since_epoch () + 5s)
           .count ())},
      source->status ().lifecycle_generation (),
      source->status ().routing_id ().to_bytes (),
      std::string ("entry"), true, {123, 456}, 0, true};
    const auto replay_instance_request = instance_request;
    assert (source->activate_instance_spot_remote (
      target->status ().routing_id (), std::move (instance_request),
      std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e',
                                0, 3, 'a', 'b', 'c'},
      {"quest.start", "application/json", {'{', '}'}}, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::reply_header_t header,
           std::optional<protocol::application_payload_t> reply_payload) {
          assert (terminal
                  == foundation::operation_terminal_t::completed);
          instance_reply_header = header;
          instance_reply_payload = std::move (reply_payload);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header);
    assert (instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload);
    assert (instance_reply_payload->packet_name == "quest.reply");
    assert (instance_prepare_count == 1);
    assert (instance_activation_count == 1);
    assert (instance_relocations->size () == 0);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source->activate_instance_spot_remote (
      target->status ().routing_id (), replay_instance_request,
      std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e',
                                0, 3, 'a', 'b', 'c'},
      {"quest.start", "application/json", {'{', '}'}}, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::reply_header_t header,
           std::optional<protocol::application_payload_t> reply_payload) {
          assert (terminal
                  == foundation::operation_terminal_t::completed);
          instance_reply_header = header;
          instance_reply_payload = std::move (reply_payload);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header
            && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload
            && instance_reply_payload->packet_name == "quest.reply");
    assert (instance_prepare_count == 1);
    assert (instance_activation_count == 1);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source->activate_instance_spot_remote (
      target->status ().routing_id (), replay_instance_request,
      std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e',
                                0, 3, 'a', 'b', 'c'},
      {"quest.start", "application/json", {'[', ']'}}, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::reply_header_t header,
           std::optional<protocol::application_payload_t> reply_payload) {
          assert (terminal
                  == foundation::operation_terminal_t::completed);
          instance_reply_header = header;
          instance_reply_payload = std::move (reply_payload);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header
            && instance_reply_header->terminal_result == 104);
    assert (
      instance_reply_header->failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::requestProtocolError));
    assert (!instance_reply_payload);
    assert (instance_prepare_count == 1);
    assert (instance_activation_count == 1);

    auto recovery_request = replay_instance_request;
    recovery_request.target.spot_id = "instance-recover";
    recovery_request.operation = {123, 457};
    recovery_request.target.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch () + 5s)
          .count ());
    fail_recovery_dispatch_once = true;
    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source->activate_instance_spot_remote (
      target->status ().routing_id (), recovery_request,
      std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e',
                                0, 3, 'a', 'b', 'c'},
      {"quest.start", "application/json", {'{', '}'}}, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::reply_header_t header,
           std::optional<protocol::application_payload_t> reply_payload) {
          assert (terminal
                  == foundation::operation_terminal_t::completed);
          instance_reply_header = header;
          instance_reply_payload = std::move (reply_payload);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header
            && instance_reply_header->terminal_result == 105);
    assert (instance_relocations->size () == 1);
    assert (instance_prepare_count == 2);
    assert (instance_activation_count == 2);

    assert (target->recover_instance_spot_activations () == 1);
    assert (instance_relocations->size () == 0);
    assert (instance_prepare_count == 3);
    assert (instance_activation_count == 3);

    instance_reply_header.reset ();
    instance_reply_payload.reset ();
    assert (source->activate_instance_spot_remote (
      target->status ().routing_id (), recovery_request,
      std::vector<std::uint8_t>{1, 1, 5, 't', 'r', 'a', 'c', 'e',
                                0, 3, 'a', 'b', 'c'},
      {"quest.start", "application/json", {'{', '}'}}, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::reply_header_t header,
           std::optional<protocol::application_payload_t> reply_payload) {
          assert (terminal
                  == foundation::operation_terminal_t::completed);
          instance_reply_header = header;
          instance_reply_payload = std::move (reply_payload);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!instance_reply_header
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (instance_reply_header
            && instance_reply_header->terminal_result == 0);
    assert (instance_reply_payload
            && instance_reply_payload->packet_name == "quest.reply");
    assert (instance_prepare_count == 3);
    assert (instance_activation_count == 3);

    const auto unix_deadline =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 100ms)
          .count ());
    protocol::user_spot_create_header_t create{
      1,
      {99, 1},
      source->status ().routing_id ().to_bytes (),
      source->status ().lifecycle_generation (),
      spot_id,
      "room",
      {reservation->fence.reservation_id,
       reservation->fence.expected_store_version,
       reservation->fence.object_generation,
       reservation->fence.authority_owner_generation,
       target->status ().routing_id ().to_bytes (),
       target->status ().lifecycle_generation (),
       reservation->fence.target.owner.owner_id,
       static_cast<std::uint64_t> (
         reservation->fence.target.owner.lease_generation),
       reservation->fence.capacity_bundle.spot_slots},
      unix_deadline};
    auto invalid_create = create;
    invalid_create.operation = {98, 1};
    invalid_create.spot_id = invalid_spot_id;
    invalid_create.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 20ms)
          .count ());
    invalid_create.reservation = {
      invalid_reservation->fence.reservation_id,
      invalid_reservation->fence.expected_store_version,
      invalid_reservation->fence.object_generation,
      invalid_reservation->fence.authority_owner_generation,
      target->status ().routing_id ().to_bytes (),
      target->status ().lifecycle_generation (),
      invalid_reservation->fence.target.owner.owner_id,
      static_cast<std::uint64_t> (
        invalid_reservation->fence.target.owner.lease_generation),
      invalid_reservation->fence.capacity_bundle.spot_slots};
    std::optional<protocol::user_spot_create_reply_t>
      invalid_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), invalid_create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          invalid_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!invalid_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (invalid_reply);
    assert (invalid_reply->header.terminal_result == 105);
    assert (materialize_count == 0);
    std::this_thread::sleep_for (80ms);
    auto mismatch_create = create;
    mismatch_create.operation = {97, 1};
    mismatch_create.stable_type = "other-room";
    mismatch_create.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 100ms)
          .count ());
    std::optional<protocol::user_spot_create_reply_t>
      mismatch_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), mismatch_create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          mismatch_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!mismatch_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (mismatch_reply);
    assert (mismatch_reply->header.terminal_result == 107);
    assert (
      mismatch_reply->header.failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::spotTypeMismatch));
    std::optional<protocol::user_spot_create_reply_t>
      replayed_mismatch_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), mismatch_create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          replayed_mismatch_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!replayed_mismatch_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (replayed_mismatch_reply);
    assert (
      replayed_mismatch_reply->header.failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::spotTypeMismatch));
    assert (materialize_count == 0);
    const auto replay_expiry_unix_ms =
      mismatch_create.deadline_unix_ms + 51;
    while (static_cast<std::uint64_t> (
             std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::system_clock::now ().time_since_epoch ())
               .count ())
           <= replay_expiry_unix_ms)
        std::this_thread::sleep_for (1ms);
    create.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 500ms)
          .count ());
    std::optional<protocol::user_spot_create_reply_t>
      create_reply;
    std::size_t create_terminal_count = 0;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          ++create_terminal_count;
          create_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!create_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (create_reply);
    assert (create_reply->header.terminal_result == 0);
    assert (
      create_reply->result
      == protocol::user_spot_create_result_t::created);
    assert (create_terminal_count == 1);
    assert (materialize_count == 1);
    assert (
      zlink::framework::runtime::
        cleanup_source_created_reservation (
          store, reserve.key, reservation->fence, true)
      == zlink::framework::runtime::
           source_creation_cleanup_t::stale);
    const auto committed_authority =
      store
        ->read_authority (
          zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *committed_snapshot =
      std::get_if<authority_snapshot_t> (
        &committed_authority);
    assert (
      committed_snapshot
      && committed_snapshot->allocation.state
           == placement_allocation_state_t::active);
    std::optional<protocol::user_spot_create_reply_t>
      replayed_create_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          replayed_create_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!replayed_create_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (replayed_create_reply);
    assert (
      replayed_create_reply->result
      == protocol::user_spot_create_result_t::created);
    assert (materialize_count == 1);

    auto capacity_create = create;
    capacity_create.operation = {99, 3};
    std::optional<protocol::user_spot_create_reply_t>
      capacity_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), capacity_create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          capacity_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!capacity_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (capacity_reply);
    //  Spec 32-framework-error-model:99-103 — operation-table saturation is
    //  wire-encoded Busy(108)+None (the target's queue state -> Unavailable at
    //  the requester), not Terminated(103), which is reserved for shutdown.
    assert (capacity_reply->header.terminal_result == 108);
    assert (capacity_reply->header.failure_code == 0);
    assert (materialize_count == 1);
    std::this_thread::sleep_for (200ms);
    auto expired_create = create;
    expired_create.operation = {99, 4};
    expired_create.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          - 1ms)
          .count ());
    std::optional<protocol::user_spot_create_reply_t>
      expired_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), expired_create, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          expired_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!expired_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (expired_reply);
    assert (expired_reply->header.terminal_result == 101);
    assert (materialize_count == 1);

    const auto authority =
      store
        ->read_authority (
          zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *ready =
      std::get_if<authority_snapshot_t> (&authority);
    assert (ready);
    assert (ready->allocation.state
            == placement_allocation_state_t::active);
    const auto wait_until_wire_time =
      [] (std::uint64_t target_unix_ms) {
          const auto bound =
            std::chrono::steady_clock::now () + 5s;
          while (std::chrono::steady_clock::now () < bound) {
              const auto now =
                static_cast<std::uint64_t> (
                  std::chrono::duration_cast<
                    std::chrono::milliseconds> (
                      std::chrono::system_clock::now ()
                        .time_since_epoch ())
                    .count ());
              if (now > target_unix_ms)
                  return true;
              std::this_thread::sleep_for (1ms);
          }
          return false;
      };
    assert (wait_until_wire_time (
      create.deadline_unix_ms + 50));

    auto capacity_filler = create;
    capacity_filler.operation = {99, 5};
    capacity_filler.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 200ms)
          .count ());
    std::optional<protocol::user_spot_create_reply_t>
      capacity_filler_reply;
    assert (source->create_user_spot_remote (
      target->status ().routing_id (), capacity_filler, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_create_reply_t reply,
           std::optional<protocol::application_payload_t>) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          capacity_filler_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!capacity_filler_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (capacity_filler_reply);

    protocol::user_spot_close_header_t expired_close{
      1,
      {99, 6},
      source->status ().routing_id ().to_bytes (),
      source->status ().lifecycle_generation (),
      {spot_id,
       ready->object_generation,
       target->status ().routing_id ().to_bytes (),
       target->status ().lifecycle_generation (),
       ready->authority_owner_generation,
       ready->store_version},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          - 1ms)
          .count ())};
    std::optional<protocol::user_spot_close_reply_t>
      expired_close_reply;
    assert (source->close_user_spot_remote (
      target->status ().routing_id (), expired_close, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_close_reply_t reply) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          expired_close_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!expired_close_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (
      expired_close_reply
      && expired_close_reply->header.terminal_result == 101
      && !expired_close_reply->closed);
    const auto after_expired_close =
      store
        ->read_authority (
          zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ();
    const auto *after_expired_snapshot =
      std::get_if<authority_snapshot_t> (&after_expired_close);
    assert (
      after_expired_snapshot
      && after_expired_snapshot->store_version
           == ready->store_version);

    // The expired cache miss must not create a terminal record. Once the
    // unrelated capacity filler expires, the same operation identity with a
    // live deadline must execute instead of failing fingerprint validation.
    assert (wait_until_wire_time (
      capacity_filler.deadline_unix_ms + 50));
    auto close = expired_close;
    close.deadline_unix_ms =
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + 5s)
          .count ());
    std::optional<protocol::user_spot_close_reply_t>
      close_reply;
    assert (source->close_user_spot_remote (
      target->status ().routing_id (), close, 5s,
      [&] (foundation::operation_terminal_t terminal,
           protocol::user_spot_close_reply_t reply) {
          assert (
            terminal
            == foundation::operation_terminal_t::completed);
          close_reply = std::move (reply);
      })
              .result ()
              .value ());
    deadline = std::chrono::steady_clock::now () + 5s;
    while (!close_reply
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (close_reply && close_reply->closed);
    assert (std::holds_alternative<authority_missing_t> (
      store
        ->read_authority (
          zlink::framework::runtime::spot_authority_key (spot_id))
        .result ()
        .value ()));
    source->close ();
    target->close ();
}

void verify_relocation_id_generation_retries_collisions ()
{
    const std::vector<protocol::relocation_id_t> candidates{
      {0, 0}, {0x11, 0x22}, {0x11, 0x22}, {0x33, 0x44}};
    std::size_t next = 0;
    zlink::framework::runtime::relocation_id_generator_t generator (
      [&] { return candidates.at (next++); });
    assert ((generator.issue ()
             == protocol::relocation_id_t{0x11, 0x22}));
    assert ((generator.issue ()
             == protocol::relocation_id_t{0x33, 0x44}));
    assert (next == candidates.size ());
}

// Cross-language failure-code alignment: relocationDataLost(35) is
// reserved for a verified checksum/assembly/digest/conflict integrity
// failure and must classify distinctly from requestFailed(17), which the
// encode side now uses for restore/factory/staging internal failures
// (complete_relocation_assembly's register_relocation_target_queue
// conflict, factory/restore exception, retried-restore-still-failing, and
// duplicate attempt-key branches — public_host_runtime.cpp). Also pins
// that a shutdown-shaped code (requestFailed) never classifies as
// data_lost, and that an unrecognized code falls back to internal_failure
// rather than being silently ignored.
void verify_relocation_failure_code_classification_is_distinct ()
{
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::relocationDataLost))
            == zlink::framework::framework_error_kind_t::data_lost);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestFailed))
            == zlink::framework::framework_error_kind_t::internal_failure);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::relocationDataLost))
            != host::classify_relocation_failure_code (
                 static_cast<std::uint32_t> (
                   protocol::framework_error_code::requestFailed)));
    // The rest of the reference table (spec 15 rows), pinned 1:1.
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestRejected))
            == zlink::framework::framework_error_kind_t::rejected);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestProtocolError))
            == zlink::framework::framework_error_kind_t::protocol_error);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::workerQueueFull))
            == zlink::framework::framework_error_kind_t::capacity_exceeded);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::workerTimedOut))
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::actorTypeMismatch))
            == zlink::framework::framework_error_kind_t::type_mismatch);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::spotTypeMismatch))
            == zlink::framework::framework_error_kind_t::type_mismatch);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::handlerNotFound))
            == zlink::framework::framework_error_kind_t::not_configured);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::routeNotConnected))
            == zlink::framework::framework_error_kind_t::unavailable);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestTargetNotFound))
            == zlink::framework::framework_error_kind_t::not_found);
    // An unrecognized wire code is internal_failure, not ignored/dropped.
    assert (host::classify_relocation_failure_code (999999u)
            == zlink::framework::framework_error_kind_t::internal_failure);
}

} // namespace

int main ()
{
    verify_session_relocation_route_retry_cadence ();
    verify_message_follow_invalidation_subscriptions_are_lifetime_safe ();
    verify_actor_calls_keep_selected_route_until_follow_notice ();
    verify_session_relocation_gateway_commit_is_atomic ();
    verify_mesh_node_role_is_available_before_local_descriptor_publish ();
    verify_mesh_stop_drains_admitted_request_completion ();
    verify_remote_bound_session_bind_classifies_retryable_outcomes ();
    verify_local_session_binding_uses_location_authority ();
    verify_same_node_bound_session_accepts_current_store_fence ();
    verify_bound_session_waits_for_current_local_actor_materialization ();
    verify_bound_session_rejects_mismatched_store_fence ();
    verify_spot_id_contract ();
    verify_spot_route_fence_admission_precedes_body_decode ();
    verify_public_host_route_cache_stops_at_owner_admission_deadline ();
    verify_entry_spot_identity_claim_is_global_and_fenced ();
    verify_user_spot_execution_mode_registration ();
    verify_self_actor_request_rejected_before_submission ();
    verify_actor_context_survives_coroutine_await ();
    verify_actor_yield_releases_spot_gate_before_reply ();
    verify_same_gate_request_rejected_before_submission ();
    verify_creation_terminal_operation_isolation ();
    verify_typed_capacity_retry_uses_second_candidate ();
    verify_global_identity_remote_create_and_generation_fence ();
    verify_membership_turns_and_independent_infrastructure ();
    verify_instance_cold_activation_only_from_intent ();
    verify_session_binding_and_terminal_once ();
    verify_session_ingress_sequence_is_scoped_by_actor_binding ();
    verify_session_route_supports_repeated_relocation ();
    verify_displaced_stream_binding_can_be_restored ();
    verify_verified_remote_stream_binding ();
    verify_message_follow_route_admission_and_suppression ();
    verify_actor_commit_is_replayable_until_deadline ();
    verify_terminal_journal_preserves_outstanding_entries ();
    verify_unbounded_actor_handoff_backlog ();
    verify_public_host_dispatches_one_application_record_per_turn ();
    verify_local_application_enqueue_wakes_dispatch_wait ();
    verify_root_location_session_seal_timeout_is_startup_snapshot ();
    verify_same_node_session_seal_waits_for_active_ingress ();
    verify_configured_session_seal_timeout_closes_actual_owner ();
    verify_location_store_accepted_record_authority ();
    verify_raw_spot_and_actor_routing ();
    verify_atomic_raw_stateful_ingress_commit ();
    verify_relocated_source_reply_failure_keeps_terminal_record ();
    verify_node_request_requires_remote_admission ();
    verify_unadmitted_request_is_rejected_without_framework_queue ();
    verify_full_owner_rejects_request_without_blocking_other_owner ();
    verify_raw_terminal_reply_relay ();
    verify_relocation_data_duplicates_are_distinct_arrivals ();
    verify_relocation_ingress_boundary_owns_saved_and_follow_only_work ();
    verify_durable_reply_relay_single_winner ();
    verify_public_host_dispatches_durable_reply_relay ();
    verify_remote_user_spot_create_close_terminal_once ();
    verify_relocation_id_generation_retries_collisions ();
    verify_relocation_failure_code_classification_is_distinct ();
    return 0;
}
