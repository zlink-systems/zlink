/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/relocation_seal_host_access.hpp"
#include "../support/relocation_seal_execution_fixture.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include <gtest/gtest.h>

namespace
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace host = rt::host;
namespace sf = rt::stateful;
namespace mesh = rt::mesh;
namespace wire = rt::protocol;

class committed_repository_t final : public rt::in_memory_location_repository_t
{
  public:
    fw::task_t<fw::authority_read_result_t> read_authority_core (fw::authority_key_t key,
                                                                 std::stop_token stop) override
    {
        if (snapshot && key.value == rt::actor_authority_key ("member").value)
            return fw::task_t<fw::authority_read_result_t> (
              fw::result_t<fw::authority_read_result_t>::success (*snapshot));
        return in_memory_location_repository_t::read_authority_core (std::move (key), stop);
    }
    std::optional<fw::authority_snapshot_t> snapshot;
};

class SpotWideTargetAdmission : public testing::TestWithParam<bool>
{
};

TEST_P (SpotWideTargetAdmission, QueuesCommittedMemberRequestsBeforeDispatchOpens)
{
    const auto local = GetParam ();
    auto descriptor = framework_relocation_seal_fixture::test_descriptor ("target");
    committed_repository_t repository;
    rt::live_location_reader_t reader (repository);
    auto lease = std::make_shared<const fw::location_owner_token_t> (
      fw::location_owner_token_t{"target-owner", 5});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor,
        .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}});
    target->configure_stateful_dispatch ([lease] { return lease; }, &reader);

    const sf::object_ref_t source{sf::object_kind_t::actor, "member", 3, 4,
                                  descriptor.mesh_name,     "source"};
    auto staged = source;
    staged.node_id = "target";
    auto committed = staged;
    committed.authority_owner_generation = 19;
    const sf::relocation_restore_identity_t identity{"relocation/member", 704, {7}};
    auto &objects = target->objects ();
    auto &dispatch = host::public_host_runtime_test_access_t::dispatch (*target);
    ASSERT_EQ (
      objects.restore_relocation ({.owner = source, .stable_type = "member"}, staged, identity)
        .result ()
        .value (),
      sf::stateful_error_t::none);

    wire::relocation_prepare_t prepare;
    prepare.target = {descriptor.node_routing_id, descriptor.lifecycle_generation, lease->owner_id,
                      static_cast<std::uint64_t> (lease->lease_generation)};
    host::public_host_runtime_test_access_t::stage_relay (*target, prepare, source, staged,
                                                          identity);
    const rt::actor_authority_payload_t row{.stable_type = "member",
                                            .actor_id = committed.key,
                                            .current_spot_id = "room",
                                            .current_spot_generation = 1,
                                            .owner_id = lease->owner_id,
                                            .owner_lease_generation =
                                              static_cast<std::uint64_t> (lease->lease_generation),
                                            .mesh_name = descriptor.mesh_name,
                                            .node_rid = fw::node_rid_t::from_string ("target"),
                                            .node_generation = descriptor.lifecycle_generation};
    repository.snapshot = fw::authority_snapshot_t{
      .payload = rt::encode_actor_authority_payload (row),
      .object_generation = committed.object_generation,
      .authority_owner_generation = committed.authority_owner_generation,
      .owner = *lease,
      .allocation = {.state = fw::placement_allocation_state_t::active,
                     .target = {descriptor.mesh_name, fw::node_rid_t::from_string ("target"),
                                descriptor.lifecycle_generation, *lease}}};

    const auto payload = [] (rt::messaging::message_kind_t kind) {
        rt::messaging::envelope_header_t header;
        header.kind = kind;
        header.channel_name = "relocation";
        header.message_name = "Probe";
        std::vector<zlink::message_t> parts;
        parts.push_back (rt::messaging::envelope_codec_t{}.encode_header (header));
        parts.push_back (zlink::message_t::from ("{}"));
        return wire::application_payload_t::from_parts (parts);
    };
    const auto body = payload (rt::messaging::message_kind_t::request);
    const auto reply = payload (rt::messaging::message_kind_t::response);
    std::vector<std::uint64_t> replies;
    const auto ingest = [&] (std::uint64_t operation, std::uint64_t generation) {
        const wire::actor_route_fence_t route{staged.key,
                                              staged.object_generation,
                                              descriptor.node_routing_id,
                                              descriptor.lifecycle_generation,
                                              generation,
                                              static_cast<std::uint64_t> (lease->lease_generation)};
        if (!local) {
            auto parts = fw::detail::backend::materialize_binding_parts (
              {wire::encode_actor_message_header (wire::command::actorRequest, std::nullopt, route,
                                                  {1, operation}, operation),
               wire::encode_application_payload (body)});
            auto original = mesh::service_mailbox_record_t::from_parts (
              mesh::service_mailbox_t::application_owner (host::owner_kind_t::actor, staged.key),
              mesh::service_mailbox_domain_t::application, std::move (parts),
              zlink::routing_id_t::from ("requester").to_bytes (), operation, 1);
            auto requester = std::make_shared<fw::mesh_node_descriptor_t> ();
            requester->owner_id = "requester-owner";
            requester->lease_generation = 8;
            original.source_original =
              std::shared_ptr<const fw::mesh_node_descriptor_t> (requester);
            const auto slot =
              fw::test::publish_stateful_claim (target->transport (), staged, std::move (original));
            const auto result =
              dispatch.ingest_claimed (staged, slot, std::nullopt).result ().value ();
            EXPECT_TRUE (result == sf::stateful_error_t::none
                         || result == sf::stateful_error_t::conflict);
            return result == sf::stateful_error_t::none;
        }
        mesh::service_mailbox_record_t original;
        original.source_original = mesh::local_request_source_t{lease, {}};
        original.application.emplace ();
        auto &application = *original.application;
        const auto rid = zlink::routing_id_t::from (descriptor.node_routing_id);
        application.owner.owner_kind = host::owner_kind_t::actor;
        application.owner.actor =
          host::public_host_runtime_t::remote_actor_ref (rid, staged.key, staged.object_generation);
        application.record.kind = host::record_kind_t::actor_request;
        application.record.source_node_rid = rid;
        application.record.actor_route = route;
        application.record.operation_id = {1, operation};
        application.record.reply_route_id = operation;
        application.record.reply_token.local_reply = [&replies, operation] (const auto &) {
            replies.push_back (operation);
            return true;
        };
        application.parts = wire::decode_application_parts (body);
        const auto slot =
          fw::test::publish_stateful_claim (target->transport (), staged, std::move (original));
        return host::public_host_runtime_test_access_t::dispatch_claim (
                 *target, slot.claim,
                 [] (const auto &, const auto &, auto, auto) {
                     ADD_FAILURE () << "Application dispatch ran while the target was recovering";
                 })
                 .result ()
                 .value ()
               == 1;
    };

    EXPECT_FALSE (ingest (1, committed.authority_owner_generation + 1));
    EXPECT_EQ (objects.pending (staged, sf::turn_domain_t::application), 0u);
    ASSERT_TRUE (ingest (2, committed.authority_owner_generation));
    ASSERT_TRUE (ingest (3, committed.authority_owner_generation));
    EXPECT_EQ (objects.pending (staged, sf::turn_domain_t::application), 2u);
    EXPECT_FALSE (dispatch.try_claim (staged).second);

    ASSERT_EQ (objects.reconcile_relocation_restore_authority (staged, committed, identity),
               sf::stateful_error_t::none);
    ASSERT_EQ (objects.commit_relocation_restore (committed, identity), sf::stateful_error_t::none);
    for (const auto operation : {2u, 3u}) {
        auto [error, delivery] = dispatch.try_claim (committed);
        ASSERT_EQ (error, sf::stateful_error_t::none);
        ASSERT_TRUE (delivery);
        EXPECT_EQ (delivery->frozen.operation.low, operation);
        EXPECT_TRUE (delivery->request);
        const auto completed = dispatch.complete (*delivery, reply);
        EXPECT_EQ (completed, local ? sf::stateful_error_t::none : sf::stateful_error_t::conflict);
    }
    EXPECT_FALSE (dispatch.try_claim (committed).second);
    if (local)
        EXPECT_EQ (replies, (std::vector<std::uint64_t>{2, 3}));
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemote, SpotWideTargetAdmission, testing::Bool ());
} // namespace
