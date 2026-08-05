/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/service_mailbox.hpp"

#include "runtime/dispatch/dispatch_limits.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{

service_mailbox_t::service_mailbox_t (
  std::size_t application_message_budget,
  std::size_t application_byte_budget,
  std::size_t infrastructure_message_budget,
  std::size_t infrastructure_byte_budget)
{
    if (application_message_budget == 0 || application_byte_budget == 0
        || infrastructure_message_budget == 0
        || infrastructure_byte_budget == 0) {
        throw std::invalid_argument ("service mailbox budgets must be positive");
    }
    _application.message_budget = application_message_budget;
    _application.byte_budget = application_byte_budget;
    _infrastructure.message_budget = infrastructure_message_budget;
    _infrastructure.byte_budget = infrastructure_byte_budget;
}

service_mailbox_t::domain_t &
service_mailbox_t::domain (service_mailbox_domain_t value)
{
    return value == service_mailbox_domain_t::application ? _application
                                                           : _infrastructure;
}

const service_mailbox_t::domain_t &
service_mailbox_t::domain (service_mailbox_domain_t value) const
{
    return value == service_mailbox_domain_t::application ? _application
                                                           : _infrastructure;
}

bool service_mailbox_t::try_enqueue (service_mailbox_record_t &&record)
{
    if (record.owner.empty () || record.parts.empty ()) {
        throw std::invalid_argument (
          "service mailbox record requires an owner and retained payload");
    }
    const auto retained = retained_bytes (record);
    if (retained.overflow) {
        return false;
    }
    std::lock_guard lock (_mutex);
    auto &target = domain (record.domain);
    if (_closed) {
        return false;
    }
    const auto owner = record.owner;
    auto &queue = target.owners[owner];
    const auto used_messages = record.domain == service_mailbox_domain_t::application
                                  ? (queue.messages > std::numeric_limits<std::size_t>::max ()
                                       - queue.active_messages
                                       ? std::numeric_limits<std::size_t>::max ()
                                       : queue.messages + queue.active_messages)
                                  : (target.messages > std::numeric_limits<std::size_t>::max ()
                                       - target.active_messages
                                       ? std::numeric_limits<std::size_t>::max ()
                                       : target.messages + target.active_messages);
    const auto used_bytes = record.domain == service_mailbox_domain_t::application
                              ? (queue.bytes > std::numeric_limits<std::size_t>::max ()
                                   - queue.active_bytes
                                   ? std::numeric_limits<std::size_t>::max ()
                                   : queue.bytes + queue.active_bytes)
                              : (target.bytes > std::numeric_limits<std::size_t>::max ()
                                   - target.active_bytes
                                   ? std::numeric_limits<std::size_t>::max ()
                                   : target.bytes + target.active_bytes);
    if (used_messages >= target.message_budget
        || used_bytes > target.byte_budget
        || retained.bytes > target.byte_budget - used_bytes) {
        return false;
    }
    queue.bytes += retained.bytes;
    ++queue.messages;
    queue.records.push_back (std::move (record));
    ++target.messages;
    target.bytes += retained.bytes;
    if (!queue.claimed && target.indexed.insert (owner).second) {
        target.ready.push_back (owner);
    }
    return true;
}

std::optional<service_mailbox_claim_t> service_mailbox_t::try_claim (
  service_mailbox_domain_t domain_value,
  std::size_t message_budget,
  std::size_t byte_budget)
{
    if (message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument ("service mailbox claim budgets must be positive");
    }
    std::lock_guard lock (_mutex);
    auto &source = domain (domain_value);
    while (!source.ready.empty ()) {
        auto owner = std::move (source.ready.front ());
        source.ready.pop_front ();
        source.indexed.erase (owner);
        auto found = source.owners.find (owner);
        if (found == source.owners.end () || found->second.claimed
            || found->second.records.empty ()) {
            continue;
        }
        return claim_owner_locked (
          source, domain_value, owner, message_budget, byte_budget);
    }
    return std::nullopt;
}

std::optional<service_mailbox_claim_t>
service_mailbox_t::try_claim_owner (
  service_mailbox_domain_t domain_value,
  const std::string &owner,
  std::size_t message_budget,
  std::size_t byte_budget)
{
    if (owner.empty () || message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument (
          "service mailbox owner claim arguments are invalid");
    }
    std::lock_guard lock (_mutex);
    auto &source = domain (domain_value);
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.claimed
        || found->second.records.empty ()) {
        return std::nullopt;
    }
    source.indexed.erase (owner);
    source.ready.erase (
      std::remove (source.ready.begin (), source.ready.end (), owner),
      source.ready.end ());
    return claim_owner_locked (
      source, domain_value, owner, message_budget, byte_budget);
}

std::optional<service_mailbox_claim_t>
service_mailbox_t::claim_owner_locked (
  domain_t &source,
  service_mailbox_domain_t domain_value,
  const std::string &owner,
  std::size_t message_budget,
  std::size_t byte_budget)
{
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.claimed
        || found->second.records.empty ()) {
        return std::nullopt;
    }
    auto &queue = found->second;
    queue.claimed = true;
    if (_next_claim_serial == 0) {
        _next_claim_serial = 1;
    }
    queue.claim_serial = _next_claim_serial++;
    service_mailbox_claim_t claim{
      owner, domain_value, queue.claim_serial, {}};
    std::size_t claimed_bytes = 0;
    while (!queue.records.empty ()
           && claim.records.size () < message_budget) {
        const auto retained = retained_bytes (queue.records.front ());
        if (retained.overflow) {
            break;
        }
        const auto next_bytes = retained.bytes;
        if (!claim.records.empty ()
            && (claimed_bytes >= byte_budget
                || next_bytes > byte_budget - claimed_bytes)) {
            break;
        }
        if (next_bytes > std::numeric_limits<std::size_t>::max () - claimed_bytes) {
            break;
        }
        claimed_bytes += next_bytes;
        queue.bytes -= next_bytes;
        --queue.messages;
        source.bytes -= next_bytes;
        --source.messages;
        queue.active_bytes += next_bytes;
        ++queue.active_messages;
        source.active_bytes += next_bytes;
        ++source.active_messages;
        claim.claimed_bytes += next_bytes;
        ++claim.claimed_messages;
        claim.records.push_back (std::move (queue.records.front ()));
        queue.records.pop_front ();
    }
    return claim;
}

service_mailbox_t::retained_size_t
service_mailbox_t::retained_bytes (const service_mailbox_record_t &record)
{
    std::size_t result = dispatch_limits::fixed_work_byte_cost;
    for (const auto &part : record.parts) {
        if (part.size () > std::numeric_limits<std::size_t>::max () - result)
            return {std::numeric_limits<std::size_t>::max (), true};
        result += part.size ();
    }
    return {result, false};
}

bool service_mailbox_t::release (const service_mailbox_claim_t &claim)
{
    std::lock_guard lock (_mutex);
    auto &target = domain (claim.domain);
    const auto found = target.owners.find (claim.owner);
    if (found == target.owners.end () || !found->second.claimed
        || found->second.claim_serial != claim.serial) {
        return false;
    }
    if (claim.claimed_messages > found->second.active_messages
        || claim.claimed_bytes > found->second.active_bytes
        || claim.claimed_messages > target.active_messages
        || claim.claimed_bytes > target.active_bytes) {
        return false;
    }
    found->second.active_messages -= claim.claimed_messages;
    found->second.active_bytes -= claim.claimed_bytes;
    target.active_messages -= claim.claimed_messages;
    target.active_bytes -= claim.claimed_bytes;
    found->second.claimed = false;
    found->second.claim_serial = 0;
    if (found->second.records.empty () && found->second.active_messages == 0) {
        target.owners.erase (found);
    } else if (target.indexed.insert (claim.owner).second) {
        target.ready.push_back (claim.owner);
    }
    return true;
}

void service_mailbox_t::close ()
{
    std::lock_guard lock (_mutex);
    _closed = true;
}

std::size_t service_mailbox_t::pending_messages (
  service_mailbox_domain_t domain_value) const
{
    std::lock_guard lock (_mutex);
    return domain (domain_value).messages;
}

std::size_t
service_mailbox_t::pending_bytes (service_mailbox_domain_t domain_value) const
{
    std::lock_guard lock (_mutex);
    return domain (domain_value).bytes;
}

} // namespace zlink::framework::runtime::mesh
