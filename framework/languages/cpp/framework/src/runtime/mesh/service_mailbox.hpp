/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <vector>

namespace zlink::framework::runtime::mesh
{

enum class service_mailbox_domain_t
{
    application,
    infrastructure
};

struct service_mailbox_record_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::vector<std::vector<std::uint8_t>> parts;
    std::vector<std::uint8_t> source_routing_id{};
    std::optional<std::uint64_t> request_sequence = std::nullopt;
    std::optional<std::uint64_t> correlation = std::nullopt;
    std::uint64_t source_node_generation = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
};

struct service_mailbox_claim_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::uint64_t serial;
    std::vector<service_mailbox_record_t> records;
    std::size_t claimed_messages = 0;
    std::size_t claimed_bytes = 0;
};

class service_mailbox_t
{
  public:
    service_mailbox_t (std::size_t application_message_budget,
                       std::size_t application_byte_budget,
                       std::size_t infrastructure_message_budget,
                       std::size_t infrastructure_byte_budget);

    bool try_enqueue (service_mailbox_record_t &&record);
    std::optional<service_mailbox_claim_t>
    try_claim (service_mailbox_domain_t domain,
               std::size_t message_budget,
               std::size_t byte_budget);
    std::optional<service_mailbox_claim_t>
    try_claim_owner (service_mailbox_domain_t domain,
                     const std::string &owner,
                     std::size_t message_budget,
                     std::size_t byte_budget);
    bool release (const service_mailbox_claim_t &claim);
    void close ();

    std::size_t pending_messages (service_mailbox_domain_t domain) const;
    std::size_t pending_bytes (service_mailbox_domain_t domain) const;

  private:
    struct owner_queue_t
    {
        std::deque<service_mailbox_record_t> records;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t active_messages = 0;
        std::size_t active_bytes = 0;
        bool claimed = false;
        std::uint64_t claim_serial = 0;
    };

    struct domain_t
    {
        std::map<std::string, owner_queue_t> owners;
        std::deque<std::string> ready;
        std::set<std::string> indexed;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t active_messages = 0;
        std::size_t active_bytes = 0;
        std::size_t message_budget = 0;
        std::size_t byte_budget = 0;
    };

    domain_t &domain (service_mailbox_domain_t value);
    const domain_t &domain (service_mailbox_domain_t value) const;
    std::optional<service_mailbox_claim_t>
    claim_owner_locked (domain_t &source,
                        service_mailbox_domain_t domain_value,
                        const std::string &owner,
                        std::size_t message_budget,
                        std::size_t byte_budget);
    struct retained_size_t
    {
        std::size_t bytes = 0;
        bool overflow = false;
    };
    static retained_size_t retained_bytes (const service_mailbox_record_t &record);

    mutable std::mutex _mutex;
    domain_t _application;
    domain_t _infrastructure;
    std::uint64_t _next_claim_serial = 1;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::mesh
