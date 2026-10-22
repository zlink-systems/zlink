/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/stateful/maintenance_runtime.hpp"
#include <stdexcept>

namespace zlink::framework::test
{

// Direct materialization tests supply the authority that confirmed their target commit.
class committed_authority_fixture_t final : public runtime::stateful::authority_relocation_port_t
{
  public:
    explicit committed_authority_fixture_t (runtime::stateful::object_ref_t target) :
        _committed{.target = std::move (target), .target_owner = {"fixture-target-owner", 1}}
    {
    }
    std::optional<runtime::stateful::authority_relocation_reference_t>
    read (runtime::stateful::object_kind_t kind, const std::string &key) override
    {
        if (_committed.target.kind != kind || _committed.target.key != key)
            return std::nullopt;
        return _committed;
    }
    runtime::stateful::authority_publish_result_t publish (const runtime::stateful::object_ref_t &,
                                                           const runtime::stateful::object_ref_t &,
                                                           location_owner_token_t,
                                                           object_creation_target_t,
                                                           std::string,
                                                           std::uint32_t,
                                                           runtime::stateful::inventory_digest_t,
                                                           std::vector<std::byte>,
                                                           std::string,
                                                           runtime::protocol::relocation_id_t,
                                                           location_owner_token_t) override
    {
        throw std::logic_error ("confirmed authority fixture does not publish");
    }
    runtime::stateful::source_relocation_settlement_t
    settle_source_relocation (const runtime::stateful::relocation_authority_fence_t &,
                              const std::vector<runtime::stateful::object_ref_t> &,
                              bool) override
    {
        throw std::logic_error ("confirmed authority fixture does not settle sources");
    }

  private:
    const runtime::stateful::authority_relocation_reference_t _committed;
};
}
