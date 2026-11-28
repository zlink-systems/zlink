/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_public_authority_fixture.hpp"
#include "runtime/stateful/public_store_adapters.hpp"

#include <gtest/gtest.h>

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::runtime::stateful;

class StandaloneActorInventory : public ::testing::TestWithParam<bool>
{
  protected:
    void SetUp () override
    {
        store.snapshot =
          m6c_test::source_relocation_authority (source, "play", std::chrono::system_clock::now ());
        prepare.relocation = {0x4718, 0x5728};
        prepare.target_attempt_generation = 3;
        prepare.coordinator = {"source-owner", 1, {'s'}, 1, store.snapshot->store_version};
        prepare.target = {{'t'}, 3, "target-owner", 2};
        prepare.source_node_routing_id = {'s'};
        prepare.source_node_generation = 1;
        prepare.application_version = 1;
    }

    void capture ()
    {
        if (GetParam ())
            spot = actor_relocation_spot_t{"target-room", 4, actor_authority_spot_kind_t::user};
        const auto fence = relocation_authority_fence_t::from_prepare (source, prepare);
        const auto version = authority
                               .begin_source_capture ({source}, fence, target.node_id,
                                                      placement.node_lifecycle_generation,
                                                      prepare.application_version, spot)
                               .result ()
                               .value ();
        ASSERT_TRUE (version);
        ASSERT_TRUE (authority.capture_source (source, *version).result ().value ());
    }

    const object_ref_t source{object_kind_t::actor, "actor", 7, 8, "play", "s"};
    const object_ref_t target{object_kind_t::actor, "actor", 7, 8, "play", "t"};
    const object_creation_target_t placement{
      "play", node_rid_t::from_string ("t"), 3, {"target-owner", 2}};
    protocol::relocation_prepare_t prepare{};
    std::optional<actor_relocation_spot_t> spot;
    m6c_test::public_memory_authority_store_t store;
    public_authority_store_adapter_t authority{store};
};

TEST_P (StandaloneActorInventory, SealedDigestMatchesPublishedAuthority)
{
    ASSERT_NO_FATAL_FAILURE (capture ());
    ASSERT_TRUE (store.snapshot->aggregate);
    const auto sealed =
      store.snapshot->aggregate->encoded.at ("relocation").at ("sourceInventorySha256");

    const auto previous = decode_direct_actor_authority_payload (store.snapshot->payload);
    ASSERT_TRUE (previous);
    ASSERT_NO_THROW (authority.verify_actor_target_inventory (source, prepare, placement, spot)
                       .result ()
                       .value ());
    ASSERT_TRUE (authority.record_target_prepared (source, prepare).result ().value ());
    const auto published =
      authority.publish (source, target, placement.owner, placement, "root", 1, {}, {},
                         prepare.coordinator.expected_authority_store_version, prepare.relocation,
                         {"source-owner", 1}, &prepare, spot);
    ASSERT_EQ (published.status, authority_publish_status_t::published);
    const auto payload = decode_direct_actor_authority_payload (store.snapshot->payload);
    ASSERT_TRUE (payload);
    EXPECT_EQ (payload->current_spot_id, spot ? spot->id : previous->current_spot_id);
    EXPECT_EQ (payload->current_spot_generation,
               spot ? spot->generation : previous->current_spot_generation);
    EXPECT_EQ (payload->current_spot_kind, spot ? spot->kind : previous->current_spot_kind);
    EXPECT_EQ (payload->owner_id, placement.owner.owner_id);
    EXPECT_EQ (payload->owner_lease_generation, placement.owner.lease_generation);
    EXPECT_EQ (payload->mesh_name, placement.mesh_name);
    EXPECT_EQ (payload->node_rid.value (), placement.node_rid.value ());
    EXPECT_EQ (payload->node_generation, placement.node_lifecycle_generation);
    const auto inventory =
      aggregate_inventory::build_tree ({{actor_authority_key (source.key),
                                         prepare.coordinator.expected_authority_store_version,
                                         authority_generation_transition_t::new_owner,
                                         store.snapshot->payload,
                                         {}}});
    EXPECT_EQ (sealed, inventory.root.at (location_record_fields::digestSha256));
}

TEST_P (StandaloneActorInventory, ChangedAuthorityCannotProceedToRestore)
{
    ASSERT_NO_FATAL_FAILURE (capture ());
    auto payload = *decode_direct_actor_authority_payload (store.snapshot->payload);
    payload.stable_type = "changed";
    store.snapshot->payload = encode_actor_authority_payload (payload);
    const auto result =
      authority.verify_actor_target_inventory (source, prepare, placement, spot).result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::data_lost);
    EXPECT_EQ (store.snapshot->aggregate->relocation->phase, "captured");
    EXPECT_FALSE (store.observed_target_owner);
}

INSTANTIATE_TEST_SUITE_P (KeepOrJoin, StandaloneActorInventory, ::testing::Bool ());
} // namespace
