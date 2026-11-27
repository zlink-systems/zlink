/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "m6c_memory_authority_fixture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace m6c_test
{

class memory_relocation_repository_t final : public relocation_store_port_t
{
  public:
    relocation_stored_t put (const std::vector<std::uint8_t> &payload,
                             std::chrono::hours retention,
                             std::chrono::steady_clock::time_point) override;

    std::optional<std::vector<std::uint8_t>> get (const std::string &reference) override;

    void remove (const std::string &reference) override;

    void erase_without_authority_change (const std::string &reference);

    std::mutex mutex;
    std::map<std::string, std::vector<std::uint8_t>> roots;
    std::vector<std::string> removed;
    std::vector<std::string> log;
    std::function<void ()> on_put;
    std::uint64_t next_reference = 0;
};

class memory_aggregate_authority_t final : public aggregate_authority_port_t
{
  public:
    explicit memory_aggregate_authority_t (std::shared_ptr<memory_authority_store_t> authority) :
        authority (std::move (authority))
    {
    }

    aggregate_publish_result_t prepare (const std::vector<object_ref_t> &sources,
                                        std::string target_node_id,
                                        zlink::framework::location_owner_token_t target_owner,
                                        std::string relocation_reference,
                                        std::uint32_t checksum_crc32c,
                                        inventory_digest_t inventory_digest,
                                        relocation_authority_fence_t) override;

    aggregate_publish_result_t commit (aggregate_relocation_fence_t fence) override;

    void abort (aggregate_relocation_fence_t fence) override;

    std::shared_ptr<memory_authority_store_t> authority;
    std::mutex mutex;
    std::vector<authority_relocation_reference_t> pending;
    std::uint64_t next_fence = 0;
    int prepare_count = 0;
    int commit_count = 0;
    bool prepared = false;
};

class settlement_authority_t final : public authority_relocation_port_t
{
  public:
    source_relocation_settlement_t
    settle_source_relocation (const relocation_authority_fence_t &fence,
                              const std::vector<object_ref_t> &participants,
                              bool preserve) override;

    authority_publish_result_t
    publish (const object_ref_t &,
             const object_ref_t &,
             zlink::framework::location_owner_token_t,
             zlink::framework::object_creation_target_t,
             std::string,
             std::uint32_t,
             inventory_digest_t,
             std::vector<std::byte> = {},
             std::string = {},
             zlink::framework::runtime::protocol::relocation_id_t = {},
             zlink::framework::location_owner_token_t = {},
             const zlink::framework::runtime::protocol::relocation_prepare_t * = nullptr) override;

    std::optional<authority_relocation_reference_t> read (object_kind_t,
                                                          const std::string &) override;

    relocation_authority_t observe_relocation (const relocation_authority_fence_t &fence) override;

    std::mutex mutex;
    std::optional<relocation_authority_fence_t> observed;
    relocation_authority_t observed_authority = relocation_authority_t::unsettled;
    relocation_authority_t preserved_authority = relocation_authority_t::unsettled;
    int preserve_calls = 0;
    int observe_calls = 0;
    int settle_after_observes = 0;
};

} // namespace m6c_test
