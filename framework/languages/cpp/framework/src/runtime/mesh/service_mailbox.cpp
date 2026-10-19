/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/mesh/service_mailbox.hpp"

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/spots/activation_admission.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{

std::string service_mailbox_t::application_owner (host::owner_kind_t kind, std::string_view id)
{
    switch (kind) {
        case host::owner_kind_t::node:
            return "node";
        case host::owner_kind_t::channel:
            return "channel:" + std::string (id);
        case host::owner_kind_t::spot:
            return std::string (detail::activation_admission_t::spot_key_prefix) + std::string (id);
        case host::owner_kind_t::actor:
            return std::string (detail::activation_admission_t::actor_key_prefix)
                   + std::string (id);
    }
    throw std::invalid_argument ("unknown application owner kind");
}

service_mailbox_t::domain_t &service_mailbox_t::domain (service_mailbox_domain_t value)
{
    return value == service_mailbox_domain_t::application ? _state->application
                                                          : _state->infrastructure;
}

const service_mailbox_t::domain_t &service_mailbox_t::domain (service_mailbox_domain_t value) const
{
    return value == service_mailbox_domain_t::application ? _state->application
                                                          : _state->infrastructure;
}

bool service_mailbox_t::try_enqueue (service_mailbox_record_t &&record,
                                     const std::function<void ()> &admitted)
{
    if (record.owner.empty () || (record.parts.empty () && !record.application)) {
        throw std::invalid_argument (
          "service mailbox record requires an owner and retained payload");
    }
    const auto retained = retained_bytes (record);
    std::unique_lock lock (_state->mutex);
    auto &target = domain (record.domain);
    if (_state->closed) {
        return false;
    }
    const auto owner = record.owner;
    auto &queue = target.owners[owner];
    if (record.domain == service_mailbox_domain_t::application && _state->application_prepare)
        _state->application_prepare (record);
    if (admitted)
        admitted ();
    queue.records.push_back (std::make_shared<service_mailbox_record_t> (std::move (record)));
    queue.bytes += retained;
    ++queue.messages;
    ++target.messages;
    target.bytes += retained;
    if (queue.phase == owner_phase_t::idle) {
        queue.phase = owner_phase_t::ready;
        target.ready.push_back (owner);
    }
    const bool notify = &target == &_state->application && !_state->application_receive_turn;
    lock.unlock ();
    if (notify)
        notify_application_ready ();
    return true;
}

std::optional<service_mailbox_claim_t> service_mailbox_t::try_claim (
  service_mailbox_domain_t domain_value, std::size_t message_budget, std::size_t byte_budget)
{
    if (message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument ("service mailbox claim budgets must be positive");
    }
    std::lock_guard lock (_state->mutex);
    auto &source = domain (domain_value);
    while (!source.ready.empty ()) {
        const auto &owner = source.ready.front ();
        auto found = source.owners.find (owner);
        if (found == source.owners.end () || found->second.phase != owner_phase_t::ready
            || found->second.records.empty ()) {
            source.ready.pop_front ();
            continue;
        }
        auto claim = claim_owner_locked (source, domain_value, owner, message_budget, byte_budget);
        source.ready.pop_front ();
        return claim;
    }
    return std::nullopt;
}

std::optional<service_mailbox_claim_t>
service_mailbox_t::try_claim_owner (service_mailbox_domain_t domain_value,
                                    const std::string &owner,
                                    std::size_t message_budget,
                                    std::size_t byte_budget)
{
    if (owner.empty () || message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument ("service mailbox owner claim arguments are invalid");
    }
    std::lock_guard lock (_state->mutex);
    auto &source = domain (domain_value);
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.active_messages != 0
        || found->second.records.empty ()) {
        return std::nullopt;
    }
    auto claim = claim_owner_locked (source, domain_value, owner, message_budget, byte_budget);
    if (found->second.phase != owner_phase_t::draining) {
        source.ready.erase (std::remove (source.ready.begin (), source.ready.end (), owner),
                            source.ready.end ());
    }
    return claim;
}

std::optional<service_mailbox_claim_t>
service_mailbox_t::claim_owner_locked (domain_t &source,
                                       service_mailbox_domain_t domain_value,
                                       const std::string &owner,
                                       std::size_t message_budget,
                                       std::size_t byte_budget)
{
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.active_messages != 0
        || found->second.records.empty ()) {
        return std::nullopt;
    }
    auto &queue = found->second;
    if (_state->next_claim_serial == 0) {
        _state->next_claim_serial = 1;
    }
    queue.claim_serial = _state->next_claim_serial++;
    service_mailbox_claim_t claim{owner, domain_value, queue.claim_serial, {}};
    std::size_t claimed_bytes = 0;
    for (const auto &slot : queue.records) {
        if (claim.records.size () == message_budget)
            break;
        const auto next_bytes = retained_bytes (*slot);
        if (!claim.records.empty ()
            && (claimed_bytes >= byte_budget || next_bytes > byte_budget - claimed_bytes)) {
            break;
        }
        if (next_bytes > std::numeric_limits<std::size_t>::max () - claimed_bytes) {
            break;
        }
        claimed_bytes += next_bytes;
        claim.claimed_bytes += next_bytes;
        ++claim.claimed_messages;
        claim.record_bytes.push_back (next_bytes);
        claim.records.push_back (slot);
    }
    for (std::size_t index = 0; index < claim.records.size (); ++index) {
        std::weak_ptr<service_mailbox_record_t> weak = claim.records[index];
        claim.records[index]->transferred = [state = std::weak_ptr (_state), weak, owner,
                                             domain_value, serial = claim.serial,
                                             bytes = claim.record_bytes[index]] {
            const auto storage = state.lock ();
            const auto slot = weak.lock ();
            if (storage && slot)
                (void) release_record (storage, domain_value, owner, serial, slot, bytes);
        };
    }
    queue.bytes -= claim.claimed_bytes;
    queue.messages -= claim.claimed_messages;
    source.bytes -= claim.claimed_bytes;
    source.messages -= claim.claimed_messages;
    queue.active_bytes += claim.claimed_bytes;
    queue.active_messages += claim.claimed_messages;
    source.active_bytes += claim.claimed_bytes;
    source.active_messages += claim.claimed_messages;
    if (queue.phase != owner_phase_t::draining)
        queue.phase = owner_phase_t::retained;
    return claim;
}

std::optional<service_mailbox_record_t> service_mailbox_t::take (const claim_handle_t &handle)
{
    std::lock_guard lock (_state->mutex);
    if (!handle || (handle->parts.empty () && !handle->application))
        return std::nullopt;
    const auto &source = domain (handle->domain);
    const auto owner = source.owners.find (handle->owner);
    if (owner == source.owners.end ())
        return std::nullopt;
    // Transfer only a prefix. Empty slots retain accounting until their owner settles.
    for (const auto &slot : owner->second.records) {
        if (slot->parts.empty () && !slot->application)
            continue;
        if (slot != handle)
            return std::nullopt;
        return std::exchange (*handle, service_mailbox_record_t{});
    }
    return std::nullopt;
}

std::size_t service_mailbox_t::retained_bytes (const service_mailbox_record_t &record)
{
    std::size_t result = dispatch_limits::fixed_work_byte_cost;
    const auto add = [&] (const auto &parts) {
        for (const auto &part : parts) {
            if (part.size () > std::numeric_limits<std::size_t>::max () - result)
                return false;
            result += part.size ();
        }
        return true;
    };
    if (!add (record.parts) || (record.application && !add (record.application->parts)))
        return std::numeric_limits<std::size_t>::max ();
    return result;
}

bool service_mailbox_t::release (const service_mailbox_claim_t &claim)
{
    return release_owned (_state, claim);
}

bool service_mailbox_t::release_owned (const std::shared_ptr<state_t> &state,
                                       const service_mailbox_claim_t &claim)
{
    bool released = false;
    for (std::size_t index = 0; index < claim.records.size (); ++index)
        released = release_record (state, claim.domain, claim.owner, claim.serial,
                                   claim.records[index], claim.record_bytes[index])
                   || released;
    return released;
}

bool service_mailbox_t::release_record (const std::shared_ptr<state_t> &state,
                                        service_mailbox_domain_t domain_value,
                                        const std::string &owner,
                                        std::uint64_t serial,
                                        const claim_handle_t &slot,
                                        std::size_t bytes)
{
    service_mailbox_record_t discarded;
    std::unique_lock lock (state->mutex);
    auto &target = domain_value == service_mailbox_domain_t::application ? state->application
                                                                         : state->infrastructure;
    const auto found = target.owners.find (owner);
    if (found == target.owners.end () || found->second.active_messages == 0
        || found->second.claim_serial != serial)
        return false;
    auto &queue = found->second;
    const auto position = std::find (queue.records.begin (), queue.records.end (), slot);
    if (position == queue.records.end ())
        return false;
    discarded = std::exchange (**position, service_mailbox_record_t{});
    queue.records.erase (position);
    --queue.active_messages;
    queue.active_bytes -= bytes;
    --target.active_messages;
    target.active_bytes -= bytes;
    if (queue.active_messages != 0)
        return true;
    queue.claim_serial = 0;
    if (queue.phase != owner_phase_t::draining) {
        if (queue.records.empty ())
            target.owners.erase (found);
        else {
            queue.phase = owner_phase_t::ready;
            target.ready.push_back (owner);
        }
    }
    const bool notify = &target == &state->application && !state->application_receive_turn;
    lock.unlock ();
    if (notify)
        notify_application_ready_owned (state);
    return true;
}

void service_mailbox_t::bind_application_dispatch (application_prepare_t prepare,
                                                   application_ready_t ready,
                                                   stateful_ready_t stateful_ready)
{
    std::lock_guard lock (_state->mutex);
    _state->application_prepare = std::move (prepare);
    _state->application_ready = std::move (ready);
    _state->stateful_ready = std::move (stateful_ready);
}

task_t<bool> service_mailbox_t::notify_stateful_ready (const std::string &owner,
                                                       stateful_take_t take)
{
    std::unique_lock lock (_state->mutex);
    auto ready = _state->stateful_ready;
    lock.unlock ();
    return ready ? ready (owner, std::move (take)) : task_t<bool> (result_t<bool>::success (false));
}

bool service_mailbox_t::has_application_dispatch () const
{
    std::lock_guard lock (_state->mutex);
    return static_cast<bool> (_state->application_ready);
}

bool service_mailbox_t::begin_application_drain (const std::string &owner)
{
    std::lock_guard lock (_state->mutex);
    const auto found = _state->application.owners.find (owner);
    if (found == _state->application.owners.end () || found->second.phase != owner_phase_t::ready)
        return false;
    found->second.phase = owner_phase_t::draining;
    return true;
}

void service_mailbox_t::end_application_drain (const std::string &owner)
{
    std::unique_lock lock (_state->mutex);
    const auto found = _state->application.owners.find (owner);
    if (found == _state->application.owners.end ()
        || found->second.phase != owner_phase_t::draining)
        return;
    auto &queue = found->second;
    if (queue.active_messages != 0) {
        queue.phase = owner_phase_t::retained;
    } else if (queue.records.empty ()) {
        _state->application.owners.erase (found);
    } else {
        queue.phase = owner_phase_t::ready;
        _state->application.ready.push_back (owner);
    }
    const bool notify = !_state->application_receive_turn;
    lock.unlock ();
    if (notify)
        notify_application_ready ();
}

void service_mailbox_t::begin_application_receive_turn ()
{
    std::lock_guard lock (_state->mutex);
    _state->application_receive_turn = true;
}

void service_mailbox_t::end_application_receive_turn ()
{
    {
        std::lock_guard lock (_state->mutex);
        _state->application_receive_turn = false;
    }
    notify_application_ready ();
}

void service_mailbox_t::notify_application_ready ()
{
    notify_application_ready_owned (_state);
}

void service_mailbox_t::notify_application_ready_owned (const std::shared_ptr<state_t> &state)
{
    std::unique_lock lock (state->mutex);
    if (state->application_receive_turn || !state->application_ready
        || state->application.ready.empty ())
        return;
    std::deque<std::string> ready;
    ready.swap (state->application.ready);
    auto notify = state->application_ready;
    lock.unlock ();
    for (const auto &owner : ready)
        notify (owner);
}

void service_mailbox_t::close ()
{
    std::lock_guard lock (_state->mutex);
    _state->closed = true;
}

std::size_t service_mailbox_t::pending_messages (service_mailbox_domain_t domain_value) const
{
    std::lock_guard lock (_state->mutex);
    return domain (domain_value).messages;
}

std::size_t service_mailbox_t::pending_bytes (service_mailbox_domain_t domain_value) const
{
    std::lock_guard lock (_state->mutex);
    return domain (domain_value).bytes;
}

} // namespace zlink::framework::runtime::mesh
