/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_state.hpp"

#include <algorithm>
#include <limits>

namespace zlink::framework::runtime::admission_queue
{
namespace
{
constexpr auto report_validity = std::chrono::seconds (3);
constexpr auto position_interval = std::chrono::seconds (5);
constexpr auto rate_window = std::chrono::milliseconds (ticket_lifetime_ms);
}

queue_time_t queue_time_t::now ()
{
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::system_clock::now ().time_since_epoch ());
    return {std::chrono::steady_clock::now (), static_cast<std::uint64_t> (wall.count ())};
}

queue_state_t::queue_state_t (std::shared_ptr<const ticket_signer_t> tickets,
                              std::optional<std::int32_t> global_limit,
                              std::uint64_t epoch) :
    _tickets (std::move (tickets)), _global_limit (global_limit), _epoch (epoch)
{
}

enqueue_reply_t queue_state_t::enqueue (connection_t connection, queue_time_t now)
{
    expire (now);
    auto place = std::ranges::find (_waiters, connection, &waiter_t::connection);
    if (place == _waiters.end ()) {
        _waiters.push_back (waiter_t{.connection = std::move (connection)});
        place = std::prev (_waiters.end ());
    }
    issue (now);
    const auto ahead =
      std::count_if (_waiters.begin (), place, [] (const auto &waiter) { return !waiter.ticket; });
    return {_epoch, deliver (*place, position_of (ahead + 1), now.steady)};
}

std::set<owner_t> queue_state_t::accept_report (const node_report_t &report, queue_time_t now)
{
    expire (now);
    const std::set held (report.held.begin (), report.held.end ());
    std::erase_if (_permits, [&] (const auto &entry) {
        const auto &[id, permit] = entry;
        return permit.node == report.node && permit.request_seq <= report.resolved_request_seq
               && !held.contains (id);
    });
    _reports.insert_or_assign (
      report.node, report_state_t{now.steady, std::max (0, report.limit - report.in_flight)});
    std::set removed (report.used.begin (), report.used.end ());
    removed.insert (report.ended.begin (), report.ended.end ());
    _waiters.remove_if ([&] (const auto &waiter) {
        return node_of (waiter.connection) == report.node
               && removed.contains (session_of (waiter.connection));
    });
    return issue (now) ? permit_owners () : std::set<owner_t>{};
}

report_reply_t queue_state_t::finish_report (const node_t &node,
                                             queue_time_t now,
                                             const std::set<owner_t> &expired)
{
    if (!expired.empty ()) {
        for (const auto &owner : expired)
            owner_lease_ended (owner);
        expire (now);
        (void) issue (now);
    }
    report_reply_t reply{_epoch, {}};
    std::size_t rank = 0;
    for (auto &waiter : _waiters) {
        if (!waiter.ticket)
            ++rank;
        if (node_of (waiter.connection) != node)
            continue;
        if (waiter.ticket) {
            if (!waiter.ticket_delivered)
                reply.deliveries.push_back (deliver (waiter, {}, now.steady));
        } else {
            const auto position = position_of (rank);
            if (waiter.last_position != position
                || now.steady - waiter.last_push >= position_interval)
                reply.deliveries.push_back (deliver (waiter, position, now.steady));
        }
    }
    return reply;
}

std::uint64_t queue_state_t::admit (const admit_request_t &request, queue_time_t now)
{
    expire (now);
    const auto place = std::ranges::find (_waiters, request.connection, &waiter_t::connection);
    if (place == _waiters.end () || !valid_ticket (*place, now.unix_ms))
        return 0;

    if (request.epoch == _epoch && _global_limit && !permit_limit_reached ()) {
        std::uint64_t id;
        do {
            id = random_nonzero_u64 ();
        } while (_permits.contains (id));
        _permits.emplace (
          id, permit_t{node_of (request.connection), request.owner, request.request_seq});
        return id;
    }
    place->ticket.reset ();
    place->last_position.reset ();
    place->ticket_delivered = false;
    const auto node = node_of (place->connection);
    const auto first_of_node = std::ranges::find_if (
      _waiters, [&] (const auto &waiter) { return node_of (waiter.connection) == node; });
    _waiters.splice (first_of_node, _waiters, place);
    return 0;
}

void queue_state_t::release (const release_t &release)
{
    const auto permit = _permits.find (release.permit_id);
    if (permit != _permits.end () && permit->second.node == release.node)
        _permits.erase (permit);
}

bool queue_state_t::permit_limit_reached () const
{
    return _global_limit && _permits.size () >= static_cast<std::size_t> (*_global_limit);
}

std::set<owner_t> queue_state_t::permit_owners_at_limit (const admit_request_t &request,
                                                         queue_time_t now) const
{
    if (request.epoch != _epoch || !permit_limit_reached ())
        return {};
    const auto waiter = std::ranges::find (_waiters, request.connection, &waiter_t::connection);
    if (waiter == _waiters.end () || !valid_ticket (*waiter, now.unix_ms))
        return {};
    return permit_owners ();
}

std::set<owner_t> queue_state_t::permit_owners () const
{
    std::set<owner_t> result;
    for (const auto &[id, permit] : _permits)
        result.insert (permit.owner);
    return result;
}

void queue_state_t::owner_lease_ended (const owner_t &owner)
{
    std::erase_if (_permits, [&] (const auto &entry) { return entry.second.owner == owner; });
}

bool queue_state_t::valid_ticket (const waiter_t &waiter, std::uint64_t now) noexcept
{
    return waiter.ticket && now < waiter.expiration;
}

void queue_state_t::expire (queue_time_t now)
{
    _waiters.remove_if (
      [&] (const auto &waiter) { return waiter.ticket && !valid_ticket (waiter, now.unix_ms); });
    std::erase_if (_reports, [&] (const auto &entry) {
        return now.steady - entry.second.received > report_validity;
    });
    while (!_issued_at.empty () && now.steady - _issued_at.front () >= rate_window)
        _issued_at.pop_front ();
}

std::size_t queue_state_t::global_budget () const
{
    if (!_global_limit)
        return _waiters.size ();
    const auto issued = std::ranges::count_if (
      _waiters, [] (const auto &waiter) { return waiter.ticket.has_value (); });
    const auto outstanding = static_cast<std::size_t> (issued) + _permits.size ();
    const auto limit = static_cast<std::size_t> (*_global_limit);
    return limit > outstanding ? limit - outstanding : 0;
}

bool queue_state_t::issue (queue_time_t now)
{
    auto available = global_budget ();
    if (!available && _permits.empty ())
        return false;
    for (auto &waiter : _waiters) {
        if (waiter.ticket)
            continue;
        const auto report = _reports.find (node_of (waiter.connection));
        if (report == _reports.end () || !report->second.budget)
            continue;
        if (!available)
            return !_permits.empty ();
        const auto expiration = now.unix_ms + ticket_lifetime_ms;
        waiter.ticket.emplace (
          issued_ticket_t{_tickets->issue (waiter.connection, _epoch, expiration),
                          static_cast<std::int32_t> (ticket_lifetime_ms)});
        waiter.expiration = expiration;
        --report->second.budget;
        --available;
        _issued_at.push_back (now.steady);
    }
    return false;
}

std::int32_t queue_state_t::estimate_wait_ms (std::int32_t position, std::size_t issued)
{
    const auto numerator = static_cast<std::uint64_t> (position) * (issued ? 30'000 : 1000);
    const auto denominator = issued ? issued : 1;
    const auto delay = numerator / denominator + (numerator % denominator != 0);
    return static_cast<std::int32_t> (
      std::min<std::uint64_t> (delay, std::numeric_limits<std::int32_t>::max ()));
}

position_t queue_state_t::position_of (std::size_t position) const
{
    const auto rank = static_cast<std::int32_t> (
      std::min (position, static_cast<std::size_t> (std::numeric_limits<std::int32_t>::max ())));
    return {rank, estimate_wait_ms (rank, _issued_at.size ())};
}

delivery_t queue_state_t::deliver (waiter_t &waiter, position_t position, steady_time_t now)
{
    if (waiter.ticket) {
        waiter.ticket_delivered = true;
        return {session_of (waiter.connection), *waiter.ticket};
    }
    waiter.last_position = position;
    waiter.last_push = now;
    return {session_of (waiter.connection), position};
}

} // namespace zlink::framework::runtime::admission_queue
