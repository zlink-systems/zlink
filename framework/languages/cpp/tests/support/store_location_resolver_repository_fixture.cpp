/* SPDX-License-Identifier: Elastic-2.0 */
#include "store_location_resolver_repository_fixture.hpp"

namespace zlink::framework::test::store_location_resolvers
{
location_owner_token_t live_owner_token (zlink::framework::location_repository_t &store,
                                         const std::string &owner_id)
{
    const auto lease = store.read_owner_lease (owner_id).result ().value ();
    const auto *found = std::get_if<zlink::framework::owner_lease_found_t> (&lease);
    if (found == nullptr)
        throw std::runtime_error ("test owner lease is not active");
    return found->token;
}

std::vector<std::byte> user_spot_authority_payload (std::string_view spot_id,
                                                    std::uint64_t generation,
                                                    std::string_view owner_id,
                                                    std::uint64_t owner_lease_generation)
{
    return zlink::framework::runtime::encode_user_spot_authority_payload (
      {.state = zlink::framework::runtime::user_spot_authority_state_t::ready,
       .stable_type = "play",
       .spot_id = std::string (spot_id),
       .owner_id = std::string (owner_id),
       .owner_lease_generation = owner_lease_generation,
       .mesh_name = "mesh-a",
       .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
       .node_generation = generation});
}

void seed_mesh_node (in_memory_location_repository_t &store,
                     std::string owner_id,
                     std::string mesh_name,
                     std::string rid,
                     std::string endpoint,
                     zlink::framework::framework_runtime_state_t state)
{
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim =
      store.claim_owner_lease (owner_id, std::chrono::seconds (30)).result ().value ();
    const auto *claimed = std::get_if<zlink::framework::owner_lease_claimed_t> (&claim);
    if (claimed == nullptr)
        throw std::runtime_error ("mesh descriptor owner seed failed");
    const auto owner = claimed->token;
    const auto result = store
                          .update_mesh_node (
                            zlink::framework::mesh_node_descriptor_t{
                              .mesh_name = std::move (mesh_name),
                              .rid = zlink::routing_id_t::from (rid),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .endpoint = std::move (endpoint),
                              .application_version = 1,
                              .object_role = zlink::framework::object_role_t::server,
                              .state = state,
                              .security_identity = "test",
                              .owner_id = owner.owner_id,
                              .lease_generation = owner.lease_generation},
                            location_write_intent_t::new_claim)
                          .result ()
                          .value ();
    if (result.status != location_write_status_t::stored)
        throw std::runtime_error ("mesh descriptor seed failed");
}
} // namespace zlink::framework::test::store_location_resolvers
