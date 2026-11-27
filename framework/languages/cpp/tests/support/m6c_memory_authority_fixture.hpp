/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/maintenance_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

std::string authority_key (object_kind_t kind, const std::string &key);

class memory_authority_store_t final : public authority_relocation_port_t
{
  public:
    zlink::framework::runtime::stateful::source_relocation_settlement_t settle_source_relocation (
      const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
      const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
      bool preserve) override;

    authority_publish_result_t
    publish (const object_ref_t &source,
             const object_ref_t &target,
             zlink::framework::location_owner_token_t target_owner,
             zlink::framework::object_creation_target_t,
             std::string relocation_reference,
             std::uint32_t checksum_crc32c,
             inventory_digest_t inventory_digest,
             std::vector<std::byte> target_application_payload = {},
             std::string = {},
             zlink::framework::runtime::protocol::relocation_id_t = {},
             zlink::framework::location_owner_token_t = {},
             const zlink::framework::runtime::protocol::relocation_prepare_t * = nullptr) override;

    std::optional<authority_relocation_reference_t> read (object_kind_t kind,
                                                          const std::string &key) override;

    std::optional<std::vector<relocation_participant_identity_t>>
    list_participant_identities () override;

    relocation_authority_t observe_relocation (const relocation_authority_fence_t &fence) override;

    std::optional<authority_relocation_reference_t> read_locked (object_kind_t kind,
                                                                 const std::string &key);

    std::mutex mutex;
    std::map<std::string, authority_relocation_reference_t> rows;
    /* Store-derived participant inventory (28 §4.2): what a production
     * Location Store serves from its authority rows. */
    std::vector<relocation_participant_identity_t> participant_identities;
    std::vector<std::string> log;
    bool force_conflict = false;
    bool throw_after_publish = false;
    bool fail_read_after_publish = false;
    bool throw_on_read = false;
    bool throw_before_publish = false;
    bool source_preserved = false;
    int publish_count = 0;
    int conflict_on_publish = 0;
};

} // namespace m6c_test
