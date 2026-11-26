/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_relocation_store_fixture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace m6c_test
{

relocation_stored_t memory_relocation_repository_t::put (const std::vector<std::uint8_t> &payload,
                                                         std::chrono::hours retention,
                                                         std::chrono::steady_clock::time_point)
{
    if (retention != std::chrono::hours (24))
        throw std::runtime_error ("unexpected retention");
    std::function<void ()> callback;
    {
        std::lock_guard lock (mutex);
        callback = on_put;
    }
    if (callback)
        callback ();
    std::lock_guard lock (mutex);
    const auto reference = "root-" + std::to_string (++next_reference);
    roots[reference] = payload;
    log.push_back ("put");
    return {reference, maintenance_runtime_t::crc32c (payload)};
}

std::optional<std::vector<std::uint8_t>>
memory_relocation_repository_t::get (const std::string &reference)
{
    std::lock_guard lock (mutex);
    const auto found = roots.find (reference);
    return found == roots.end () ? std::optional<std::vector<std::uint8_t>>{}
                                 : std::make_optional (found->second);
}

void memory_relocation_repository_t::remove (const std::string &reference)
{
    std::lock_guard lock (mutex);
    roots.erase (reference);
    removed.push_back (reference);
    log.push_back ("remove");
}

void memory_relocation_repository_t::erase_without_authority_change (const std::string &reference)
{
    std::lock_guard lock (mutex);
    roots.erase (reference);
}

aggregate_publish_result_t
memory_aggregate_authority_t::prepare (const std::vector<object_ref_t> &sources,
                                       std::string target_node_id,
                                       zlink::framework::location_owner_token_t target_owner,
                                       std::string relocation_reference,
                                       std::uint32_t checksum_crc32c,
                                       inventory_digest_t inventory_digest,
                                       relocation_authority_fence_t)
{
    std::lock_guard lock (mutex);
    ++prepare_count;
    if (sources.size () < 2 || prepared)
        return {aggregate_publish_status_t::conflict, {}, {}};
    pending.clear ();
    for (const auto &source : sources) {
        auto target = source;
        target.node_id = target_node_id;
        ++target.authority_owner_generation;
        pending.push_back ({.source = source,
                            .target = target,
                            .relocation_reference = relocation_reference,
                            .checksum_crc32c = checksum_crc32c,
                            .inventory_digest = inventory_digest,
                            .target_owner = target_owner});
    }
    prepared = true;
    return {aggregate_publish_status_t::prepared, {++next_fence}, pending};
}

aggregate_publish_result_t memory_aggregate_authority_t::commit (aggregate_relocation_fence_t fence)
{
    std::lock_guard lock (mutex);
    ++commit_count;
    if (!prepared || fence.value != next_fence)
        return {aggregate_publish_status_t::conflict, fence, {}};
    {
        std::lock_guard authority_lock (authority->mutex);
        for (const auto &reference : pending) {
            authority->rows[authority_key (reference.source.kind, reference.source.key)] =
              reference;
        }
    }
    prepared = false;
    return {aggregate_publish_status_t::committed, fence, pending};
}

void memory_aggregate_authority_t::abort (aggregate_relocation_fence_t fence)
{
    std::lock_guard lock (mutex);
    if (fence.value == next_fence)
        prepared = false;
}

source_relocation_settlement_t
settlement_authority_t::settle_source_relocation (const relocation_authority_fence_t &fence,
                                                  const std::vector<object_ref_t> &participants,
                                                  bool preserve)
{
    std::lock_guard lock (mutex);
    observed = fence;
    relocation_authority_t outcome;
    if (preserve) {
        ++preserve_calls;
        outcome = preserved_authority;
    } else if (settle_after_observes != 0 && ++observe_calls < settle_after_observes)
        outcome = relocation_authority_t::unsettled;
    else
        outcome = observed_authority;
    source_relocation_settlement_t settled{outcome, {}};
    if (outcome == relocation_authority_t::target_committed)
        for (const auto &participant : participants)
            settled.follow_generations.emplace (std::pair{participant.kind, participant.key},
                                                participant.authority_owner_generation);
    return settled;
}

authority_publish_result_t
settlement_authority_t::publish (const object_ref_t &,
                                 const object_ref_t &,
                                 zlink::framework::location_owner_token_t,
                                 zlink::framework::object_creation_target_t,
                                 std::string,
                                 std::uint32_t,
                                 inventory_digest_t,
                                 std::vector<std::byte>,
                                 std::string,
                                 zlink::framework::runtime::protocol::relocation_id_t,
                                 zlink::framework::location_owner_token_t,
                                 const zlink::framework::runtime::protocol::relocation_prepare_t *)
{
    return {};
}

std::optional<authority_relocation_reference_t> settlement_authority_t::read (object_kind_t,
                                                                              const std::string &)
{
    return std::nullopt;
}

relocation_authority_t
settlement_authority_t::observe_relocation (const relocation_authority_fence_t &fence)
{
    std::lock_guard lock (mutex);
    observed = fence;
    if (settle_after_observes != 0 && ++observe_calls < settle_after_observes)
        return relocation_authority_t::unsettled;
    return observed_authority;
}

} // namespace m6c_test
