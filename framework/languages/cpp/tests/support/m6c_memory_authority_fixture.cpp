/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_memory_authority_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace m6c_test
{

std::string authority_key (object_kind_t kind, const std::string &key)
{
    return std::to_string (static_cast<int> (kind)) + ":" + key;
}

zlink::framework::runtime::stateful::source_relocation_settlement_t
memory_authority_store_t::settle_source_relocation (
  const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
  const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
  bool preserve)
{
    namespace sf = zlink::framework::runtime::stateful;
    (void) preserve;
    sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
    if (settled.outcome == sf::relocation_authority_t::target_committed) {
        for (const auto &participant : participants) {
            if (const auto current = read (participant.kind, participant.key))
                settled.follow_generations.emplace (std::pair{participant.kind, participant.key},
                                                    current->target.authority_owner_generation);
        }
    }
    return settled;
}

authority_publish_result_t memory_authority_store_t::publish (
  const object_ref_t &source,
  const object_ref_t &target,
  zlink::framework::location_owner_token_t target_owner,
  zlink::framework::object_creation_target_t,
  std::string relocation_reference,
  std::uint32_t checksum_crc32c,
  inventory_digest_t inventory_digest,
  std::vector<std::byte> target_application_payload,
  std::string,
  zlink::framework::runtime::protocol::relocation_id_t,
  zlink::framework::location_owner_token_t,
  const zlink::framework::runtime::protocol::relocation_prepare_t *)
{
    std::lock_guard lock (mutex);
    log.push_back ("publish");
    ++publish_count;
    if (throw_before_publish)
        throw std::runtime_error ("authority store response lost before commit");
    if (force_conflict || (conflict_on_publish != 0 && publish_count == conflict_on_publish))
        return {authority_publish_status_t::conflict, read_locked (source.kind, source.key)};
    auto issued_target = target;
    issued_target.authority_owner_generation = source.authority_owner_generation + 1;
    authority_relocation_reference_t reference{
      .source = source,
      .target = issued_target,
      .relocation_reference = std::move (relocation_reference),
      .checksum_crc32c = checksum_crc32c,
      .inventory_digest = inventory_digest,
      .target_owner = std::move (target_owner),
      .application_payload = std::move (target_application_payload)};
    rows[authority_key (source.kind, source.key)] = reference;
    if (throw_after_publish) {
        if (fail_read_after_publish)
            throw_on_read = true;
        throw std::runtime_error ("response lost after authority commit");
    }
    return {authority_publish_status_t::published, reference};
}

std::optional<authority_relocation_reference_t>
memory_authority_store_t::read (object_kind_t kind, const std::string &key)
{
    std::lock_guard lock (mutex);
    if (throw_on_read)
        throw std::runtime_error ("authority store unavailable");
    return read_locked (kind, key);
}

std::optional<std::vector<relocation_participant_identity_t>>
memory_authority_store_t::list_participant_identities ()
{
    std::lock_guard lock (mutex);
    if (participant_identities.empty ())
        return std::nullopt;
    return participant_identities;
}

relocation_authority_t
memory_authority_store_t::observe_relocation (const relocation_authority_fence_t &fence)
{
    {
        std::lock_guard lock (mutex);
        if (source_preserved)
            return relocation_authority_t::source_preserved;
    }
    return authority_relocation_port_t::observe_relocation (fence);
}

std::optional<authority_relocation_reference_t>
memory_authority_store_t::read_locked (object_kind_t kind, const std::string &key)
{
    const auto found = rows.find (authority_key (kind, key));
    return found == rows.end () ? std::optional<authority_relocation_reference_t>{}
                                : std::make_optional (found->second);
}

} // namespace m6c_test
