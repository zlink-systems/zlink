/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/mesh/service_mailbox.hpp"

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/spots/activation_admission.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{
namespace
{
bool has_original (const service_mailbox_record_t &record) noexcept
{
    return !record.parts.empty () || record.application;
}
} // namespace

void service_mailbox_t::start_source_lookup (const request_source_lookup_t &lookup) const
{
    request_source_lookup_state_t::start_t start;
    {
        std::lock_guard lock (_state->mutex);
        start = std::exchange (lookup->start, {});
    }
    if (start)
        start (lookup);
}


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
    if (record.owner.empty () || !has_original (record)) {
        throw std::invalid_argument (
          "service mailbox record requires an owner and retained payload");
    }
    // A queued original carries no claim credit; take mints it.
    record.transferred = {};
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
    queue.records.push_back (std::move (record));
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

std::shared_ptr<service_mailbox_claim_t>
service_mailbox_t::try_claim (service_mailbox_domain_t domain_value,
                              std::size_t message_budget,
                              std::size_t byte_budget,
                              const claimed_t &claimed)
{
    if (message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument ("service mailbox claim budgets must be positive");
    }
    std::unique_lock lock (_state->mutex);
    auto &source = domain (domain_value);
    while (!source.ready.empty ()) {
        const auto &owner = source.ready.front ();
        auto found = source.owners.find (owner);
        if (found == source.owners.end () || found->second.phase != owner_phase_t::ready
            || found->second.records.empty ()) {
            source.ready.pop_front ();
            continue;
        }
        auto claim = claim_owner_locked (source, domain_value, owner, message_budget, byte_budget,
                                         claimed, lock);
        source.ready.pop_front ();
        return claim;
    }
    return nullptr;
}

std::shared_ptr<service_mailbox_claim_t>
service_mailbox_t::try_claim_owner (service_mailbox_domain_t domain_value,
                                    const std::string &owner,
                                    std::size_t message_budget,
                                    std::size_t byte_budget,
                                    const claimed_t &claimed)
{
    if (owner.empty () || message_budget == 0 || byte_budget == 0) {
        throw std::invalid_argument ("service mailbox owner claim arguments are invalid");
    }
    std::unique_lock lock (_state->mutex);
    auto &source = domain (domain_value);
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.active_messages != 0
        || found->second.records.empty ()) {
        return nullptr;
    }
    auto claim =
      claim_owner_locked (source, domain_value, owner, message_budget, byte_budget, claimed, lock);
    if (found->second.phase != owner_phase_t::draining) {
        source.ready.erase (std::remove (source.ready.begin (), source.ready.end (), owner),
                            source.ready.end ());
    }
    return claim;
}

std::shared_ptr<service_mailbox_claim_t>
service_mailbox_t::claim_owner_locked (domain_t &source,
                                       service_mailbox_domain_t domain_value,
                                       const std::string &owner,
                                       std::size_t message_budget,
                                       std::size_t byte_budget,
                                       const claimed_t &claimed,
                                       std::unique_lock<std::mutex> &lock)
{
    const auto found = source.owners.find (owner);
    if (found == source.owners.end () || found->second.active_messages != 0
        || found->second.records.empty ()) {
        return nullptr;
    }
    auto &queue = found->second;
    auto claim = std::make_shared<service_mailbox_claim_t> ();
    claim->owner = owner;
    claim->domain = domain_value;
    const auto limit = std::min (message_budget, queue.records.size ());
    claim->records.reserve (limit);
    claim->record_bytes.reserve (limit);
    // The claim stays the front prefix of the FIFO; list references survive push_back.
    for (auto &slot : queue.records) {
        if (claim->records.size () == limit)
            break;
        const auto next_bytes = retained_bytes (slot);
        if (!claim->records.empty ()
            && (claim->claimed_bytes >= byte_budget
                || next_bytes > byte_budget - claim->claimed_bytes)) {
            break;
        }
        if (next_bytes > std::numeric_limits<std::size_t>::max () - claim->claimed_bytes) {
            break;
        }
        claim->claimed_bytes += next_bytes;
        claim->record_bytes.push_back (next_bytes);
        claim->records.push_back (&slot);
    }
    claim->claimed_messages = claim->records.size ();
    claim->_mailbox = _state;
    claim->_settled = std::make_unique<std::atomic_bool[]> (claim->claimed_messages);
    claim->_remaining.store (claim->claimed_messages, std::memory_order_relaxed);
    // Each slot holds its own credit; whichever take moves the original out holds it.
    for (std::size_t index = 0; index != claim->claimed_messages; ++index)
        claim->records[index]->transferred = service_mailbox_settlement_t (claim, index);
    queue.bytes -= claim->claimed_bytes;
    queue.messages -= claim->claimed_messages;
    source.bytes -= claim->claimed_bytes;
    source.messages -= claim->claimed_messages;
    queue.active_bytes += claim->claimed_bytes;
    queue.active_messages += claim->claimed_messages;
    source.active_bytes += claim->claimed_bytes;
    source.active_messages += claim->claimed_messages;
    if (queue.phase != owner_phase_t::draining)
        queue.phase = owner_phase_t::retained;
    // A consumer allocation may fail before or during transfer. Return every
    // outstanding credit through the same release owner. Own the remaining originals
    // before unlocking so a relocation cut cannot acquire credits being returned.
    const auto rollback = [this, &lock, &claim] (service_mailbox_claim_t *failed) {
        std::vector<service_mailbox_record_t> discarded;
        discarded.reserve (failed->claimed_messages);
        for (std::size_t index = 0; index != failed->claimed_messages; ++index)
            if (auto *slot = slot_locked ({claim, index}))
                discarded.push_back (transfer_locked (*slot));
        lock.unlock ();
        release (*failed);
    };
    std::unique_ptr<service_mailbox_claim_t, decltype (rollback)> pending (claim.get (), rollback);
    if (claimed)
        claimed (claim);
    pending.release ();
    return claim;
}

service_mailbox_record_t *service_mailbox_t::slot_locked (const claim_handle_t &handle)
{
    // A claim whose credits all settled has left the FIFO; its view is not read.
    if (!handle.claim || handle.claim->_remaining.load (std::memory_order_acquire) == 0
        || handle.index >= handle.claim->records.size ())
        return nullptr;
    auto *slot = handle.claim->records[handle.index];
    return has_original (*slot) ? slot : nullptr;
}

bool service_mailbox_t::next_locked (const claim_handle_t &handle)
{
    // Transfers leave a claim in FIFO order: the previous slot is already empty.
    if (handle.index == 0)
        return true;
    const auto *previous = handle.claim->records[handle.index - 1];
    return !has_original (*previous);
}

service_mailbox_record_t service_mailbox_t::transfer_locked (service_mailbox_record_t &slot)
{
    // Moving out leaves the slot without parts or application: the taken mark. The
    // slot's claim credit moves with the original.
    return std::move (slot);
}

std::optional<std::vector<std::vector<service_mailbox_record_t>>>
service_mailbox_t::cut_owners (const std::vector<std::string> &owners,
                               const cut_admits_t &admits,
                               const std::function<void ()> &cut)
{
    std::vector<std::vector<service_mailbox_record_t>> taken (owners.size ());
    std::unique_lock lock (_state->mutex);
    auto &source = _state->application;
    for (const auto &owner : owners) {
        const auto found = source.owners.find (owner);
        if (found == source.owners.end ())
            continue;
        for (const auto &slot : found->second.records)
            if (has_original (slot) && admits && !admits (slot))
                return std::nullopt;
    }
    for (std::size_t index = 0; index != owners.size (); ++index) {
        const auto found = source.owners.find (owners[index]);
        if (found == source.owners.end ())
            continue;
        auto &queue = found->second;
        auto &records = taken[index];
        records.reserve (queue.records.size ());
        // The claimed prefix keeps its slots until the claim settles; the remaining
        // unclaimed records leave the FIFO now.
        const auto claimed = std::min (queue.active_messages, queue.records.size ());
        auto unclaimed = queue.records.begin ();
        for (std::size_t index = 0; index != claimed; ++index, ++unclaimed)
            if (has_original (*unclaimed))
                records.push_back (transfer_locked (*unclaimed));
        for (auto slot = unclaimed; slot != queue.records.end (); ++slot) {
            const auto bytes = retained_bytes (*slot);
            queue.bytes -= bytes;
            source.bytes -= bytes;
            --queue.messages;
            --source.messages;
            records.push_back (transfer_locked (*slot));
        }
        queue.records.erase (unclaimed, queue.records.end ());
        if (queue.records.empty () && queue.active_messages == 0
            && queue.phase != owner_phase_t::draining)
            source.owners.erase (found);
    }
    if (cut)
        cut ();
    return taken;
}

std::optional<service_mailbox_record_t> service_mailbox_t::take (const claim_handle_t &handle)
{
    std::lock_guard lock (_state->mutex);
    auto *slot = slot_locked (handle);
    if (!slot || !next_locked (handle))
        return std::nullopt;
    return transfer_locked (*slot);
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

bool service_mailbox_t::release (service_mailbox_claim_t &claim)
{
    std::vector<service_mailbox_record_t> discarded;
    std::unique_lock lock (_state->mutex);
    std::size_t settled = 0;
    for (std::size_t index = 0; index < claim.claimed_messages; ++index) {
        if (!settle_once (claim, index))
            continue;
        ++settled;
        auto &slot = *claim.records[index];
        if (has_original (slot))
            discarded.push_back (transfer_locked (slot));
    }
    if (settled == 0)
        return false;
    bool notify = false;
    if (settles_last (claim, settled))
        notify = finish_locked (*_state, claim);
    lock.unlock ();
    discarded.clear ();
    if (notify)
        notify_application_ready ();
    return true;
}

bool service_mailbox_t::settle_once (service_mailbox_claim_t &claim, std::size_t index) noexcept
{
    // The single settlement decision: each slot credit settles once.
    return !claim._settled[index].exchange (true, std::memory_order_acq_rel);
}

bool service_mailbox_t::settles_last (service_mailbox_claim_t &claim, std::size_t count) noexcept
{
    return claim._remaining.fetch_sub (count, std::memory_order_acq_rel) == count;
}

bool service_mailbox_t::settle (const service_mailbox_settlement_t &settlement)
{
    // The last credit returns the claim to its owner FIFO under one lock.
    auto &claim = *settlement._claim;
    if (!settle_once (claim, settlement._index))
        return false;
    if (!settles_last (claim, 1))
        return true;
    const auto state = claim._mailbox.lock ();
    if (!state)
        return true;
    std::unique_lock lock (state->mutex);
    const bool notify = finish_locked (*state, claim);
    lock.unlock ();
    if (notify)
        notify_application_ready_owned (state);
    return true;
}

bool service_mailbox_t::finish_locked (state_t &state, const service_mailbox_claim_t &claim)
{
    auto &target = claim.domain == service_mailbox_domain_t::application ? state.application
                                                                         : state.infrastructure;
    const auto found = target.owners.find (claim.owner);
    if (found == target.owners.end ())
        return false;
    auto &queue = found->second;
    // Every slot of the prefix was taken or discarded before its credit settled.
    queue.records.erase (queue.records.begin (),
                         std::next (queue.records.begin (), claim.claimed_messages));
    queue.active_messages -= claim.claimed_messages;
    queue.active_bytes -= claim.claimed_bytes;
    target.active_messages -= claim.claimed_messages;
    target.active_bytes -= claim.claimed_bytes;
    if (queue.phase != owner_phase_t::draining) {
        if (queue.records.empty ())
            target.owners.erase (found);
        else {
            queue.phase = owner_phase_t::ready;
            target.ready.push_back (claim.owner);
        }
    }
    return &target == &state.application && !state.application_receive_turn;
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
    auto ready = take_application_receive_turn ();
    notify_application_ready (ready);
}

service_mailbox_t::application_ready_batch_t service_mailbox_t::take_application_receive_turn ()
{
    std::lock_guard lock (_state->mutex);
    _state->application_receive_turn = false;
    return take_application_ready_locked (*_state);
}

service_mailbox_t::application_ready_batch_t
service_mailbox_t::take_application_ready_locked (state_t &state)
{
    if (state.application_receive_turn || !state.application_ready
        || state.application.ready.empty ())
        return std::nullopt;
    application_ready_batch_t ready (std::in_place);
    ready->second = state.application_ready;
    ready->first.swap (state.application.ready);
    return ready;
}

void service_mailbox_t::notify_application_ready (application_ready_batch_t &ready)
{
    while (ready && !ready->first.empty ()) {
        auto owner = std::move (ready->first.front ());
        ready->first.pop_front ();
        ready->second (owner);
    }
}

void service_mailbox_t::notify_application_ready ()
{
    notify_application_ready_owned (_state);
}

void service_mailbox_t::notify_application_ready_owned (const std::shared_ptr<state_t> &state)
{
    std::unique_lock lock (state->mutex);
    auto ready = take_application_ready_locked (*state);
    lock.unlock ();
    notify_application_ready (ready);
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
