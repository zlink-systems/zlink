/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <zlink/Contracts/Messaging/received.hpp>
#include "runtime/dispatch/application_record.hpp"

namespace zlink::framework::runtime::mesh
{

enum class service_mailbox_domain_t
{
    application,
    infrastructure
};

enum class service_mailbox_enqueue_result_t
{
    accepted,
    capacity_exceeded,
    closed
};

struct service_bound_session_source_t
{
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t binding_generation = 0;
    std::uint64_t session_sequence = 0;
};

struct service_mailbox_record_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::vector<std::vector<std::uint8_t>> parts;
    std::vector<std::uint8_t> source_routing_id{};
    std::optional<zlink::reply_token_t> reply_token = std::nullopt;
    std::optional<std::uint64_t> correlation = std::nullopt;
    std::uint64_t source_node_generation = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
    std::optional<service_bound_session_source_t> bound_session_source;
    std::function<void ()> before_application_handler;
    std::shared_ptr<host::local_application_dispatch_t> application;
};

struct service_mailbox_claim_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::uint64_t serial;
    std::vector<service_mailbox_record_t> records;
    std::vector<std::size_t> record_bytes;
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

    static std::string application_owner (host::owner_kind_t kind, std::string_view id = {});
    static std::string application_owner (const host::ready_record_t &owner);

    using application_prepare_t = std::function<void (service_mailbox_record_t &)>;
    using application_ready_t = std::function<void (const std::string &)>;
    void bind_application_dispatch (application_prepare_t prepare, application_ready_t ready);
    bool has_application_dispatch () const;
    void begin_application_receive_turn ();
    void end_application_receive_turn ();
    bool begin_application_drain (const std::string &owner);
    void end_application_drain (const std::string &owner);

    bool try_enqueue (service_mailbox_record_t &&record);
    service_mailbox_enqueue_result_t try_enqueue_result (
      service_mailbox_record_t &&record);
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
    enum class owner_phase_t { idle, ready, draining, retained };

    struct owner_queue_t
    {
        std::deque<service_mailbox_record_t> records;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t active_messages = 0;
        std::size_t active_bytes = 0;
        owner_phase_t phase = owner_phase_t::idle;
        std::uint64_t claim_serial = 0;
    };

    struct domain_t
    {
        std::unordered_map<std::string, owner_queue_t> owners;
        std::deque<std::string> ready;
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

    void notify_application_ready ();

    mutable std::mutex _mutex;
    domain_t _application;
    domain_t _infrastructure;
    std::uint64_t _next_claim_serial = 1;
    bool _closed = false;
    application_prepare_t _application_prepare;
    application_ready_t _application_ready;
    bool _application_receive_turn = false;
};

inline std::string service_mailbox_t::application_owner (const host::ready_record_t &owner)
{
    switch (owner.owner_kind) {
        case host::owner_kind_t::node: return application_owner (owner.owner_kind);
        case host::owner_kind_t::channel: return application_owner (owner.owner_kind, owner.channel_name);
        case host::owner_kind_t::spot: return application_owner (owner.owner_kind, owner.spot_id);
        case host::owner_kind_t::actor:
            if (owner.actor)
                return application_owner (owner.owner_kind, owner.actor->actor_id ().value ());
            throw std::invalid_argument ("application actor owner requires its reference");
    }
    throw std::invalid_argument ("unknown application owner kind");
}

} // namespace zlink::framework::runtime::mesh
