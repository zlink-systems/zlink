/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_public_authority_fixture.hpp"
#include "runtime/locations/actor_authority_payload.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

namespace m6c_test
{

zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
public_memory_authority_store_t::read_owner_lease (std::string owner_id)
{
    if (fail_owner_lease_read)
        throw std::runtime_error ("scripted lease read failure");
    if (owner_lease_read)
        return completed (*owner_lease_read);
    return zlink::framework::runtime::in_memory_location_repository_t::read_owner_lease (
      std::move (owner_id));
}

zlink::framework::task_t<zlink::framework::authority_read_result_t>
public_memory_authority_store_t::read_authority_core (zlink::framework::authority_key_t key,
                                                      std::stop_token)
{
    if (!authority_rows.empty ()) {
        const auto current = authority_rows.find (key.value);
        if (current != authority_rows.end ())
            return completed (zlink::framework::authority_read_result_t{current->second});
    }
    observed_keys.push_back (std::move (key.value));
    if (!snapshot)
        return completed (zlink::framework::authority_read_result_t{
          zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}});
    return completed (zlink::framework::authority_read_result_t{*snapshot});
}

zlink::framework::task_t<zlink::framework::authority_compare_exchange_result_t>
public_memory_authority_store_t::compare_exchange_authority_core (
  zlink::framework::authority_key_t key,
  std::string expected_store_version,
  zlink::framework::authority_mutation_t mutation,
  std::stop_token)
{
    observed_keys.push_back (std::move (key.value));
    const auto *put = std::get_if<zlink::framework::authority_put_t> (&mutation);
    const auto *retarget = std::get_if<zlink::framework::authority_retarget_t> (&mutation);
    if (!snapshot || (!put && !retarget) || expected_store_version != snapshot->store_version)
        return completed (zlink::framework::authority_compare_exchange_result_t{
          zlink::framework::authority_conflict_t{
            snapshot
              ? zlink::framework::authority_read_result_t{*snapshot}
              : zlink::framework::authority_read_result_t{
                  zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}}});
    if (remaining_authority_conflicts != 0) {
        --remaining_authority_conflicts;
        if (conflict_snapshot) {
            snapshot = conflict_snapshot;
            return completed (zlink::framework::authority_compare_exchange_result_t{
              zlink::framework::authority_conflict_t{
                zlink::framework::authority_read_result_t{*snapshot}}});
        }
        snapshot->store_version = std::to_string (std::stoull (snapshot->store_version) + 1);
        snapshot->store_now = std::chrono::system_clock::now ();
        return completed (zlink::framework::authority_compare_exchange_result_t{
          zlink::framework::authority_conflict_t{
            zlink::framework::authority_read_result_t{*snapshot}}});
    }
    if (retarget) {
        observed_target_owner = retarget->target.owner;
        observed_target_placement = retarget->target;
        snapshot->authority_owner_generation += retarget_authority_generation_advance;
        snapshot->owner = retarget->target.owner;
        snapshot->allocation.target = retarget->target;
        snapshot->payload = retarget->payload;
        if (retarget->replace_aggregate)
            snapshot->aggregate = *retarget->replace_aggregate;
    } else {
        snapshot->payload = put->payload;
        if (put->replace_aggregate)
            snapshot->aggregate = *put->replace_aggregate;
    }
    snapshot->store_version = std::to_string (std::stoull (snapshot->store_version) + 1);
    snapshot->store_now = std::chrono::system_clock::now ();
    return completed (zlink::framework::authority_compare_exchange_result_t{
      zlink::framework::authority_stored_t{*snapshot}});
}

zlink::framework::task_t<zlink::framework::authority_scan_result_t>
public_memory_authority_store_t::list_authorities (
  std::string,
  std::optional<zlink::framework::authority_scan_cursor_t>,
  std::size_t,
  std::stop_token)
{
    return completed (
      zlink::framework::authority_scan_result_t{zlink::framework::authority_page_t{}});
}

zlink::framework::authority_snapshot_t source_relocation_authority (
  const object_ref_t &actor, std::string mesh, std::chrono::system_clock::time_point now)
{
    namespace fw = zlink::framework;
    const fw::object_creation_target_t placement{
      std::move (mesh), fw::node_rid_t::from_string ("s"), 1, {"source-owner", 1}};
    auto payload = fw::runtime::encode_actor_authority_payload (
      {.stable_type = "player",
       .actor_id = actor.key,
       .current_spot_id = "source-entry",
       .current_spot_generation = 1,
       .current_spot_kind = fw::runtime::actor_authority_spot_kind_t::entry,
       .owner_id = placement.owner.owner_id,
       .owner_lease_generation = static_cast<std::uint64_t> (placement.owner.lease_generation),
       .mesh_name = placement.mesh_name,
       .node_rid = placement.node_rid,
       .node_generation = placement.node_lifecycle_generation});
    fw::authority_snapshot_t snapshot{"1",
                                      std::move (payload),
                                      actor.object_generation,
                                      actor.authority_owner_generation,
                                      placement.owner,
                                      now};
    snapshot.allocation.target = placement;
    return snapshot;
}

} // namespace m6c_test
