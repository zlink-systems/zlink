/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_state.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace zlink::framework::runtime::admission_queue
{
namespace
{
using namespace std::chrono_literals;

bytes_t bytes (std::string_view value)
{
    const auto raw = std::as_bytes (std::span (value));
    return {raw.begin (), raw.end ()};
}

std::string text (const bytes_t &value)
{
    return {reinterpret_cast<const char *> (value.data ()), value.size ()};
}

connection_t connection (const json_t &value, std::string fallback_node = "n")
{
    if (value.is_string ())
        return {bytes (fallback_node), 1, "login", bytes (value.get<std::string> ())};
    return {bytes (value.value ("node", fallback_node)),
            value.value ("nodeLifecycleGeneration", 1u),
            value.value ("streamName", std::string ("login")),
            bytes (value.value ("sessionRid", std::string ("a")))};
}

session_t session (const json_t &value)
{
    return session_of (connection (value));
}

std::optional<std::int32_t> global_limit (const json_t &value)
{
    if (value.contains ("globalLimit"))
        return value.at ("globalLimit").get<std::int32_t> ();
    return {};
}
} // namespace

struct queue_fixture_t
{
    explicit queue_fixture_t (const json_t &entry) :
        fixture (entry),
        given (entry.at ("given")),
        signer (std::make_shared<ticket_signer_t> ("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
                                                   std::nullopt)),
        state (signer, global_limit (given), given.value ("epoch", 42u)),
        now{std::chrono::steady_clock::time_point (10s), 1'700'000'000'000}
    {
        for (const auto &row : given.value ("waiting", json_t::array ())) {
            auto &waiter = wait (connection (row));
            if (row.is_object () && row.value ("ticket", false))
                seed_ticket (waiter, now.unix_ms + ticket_lifetime_ms);
        }
        for (const auto &row : given.value ("waiters", json_t::array ()))
            wait (connection (row));
        for (const auto &row : given.value ("validTickets", json_t::array ()))
            seed_ticket (wait (connection (row)), now.unix_ms + ticket_lifetime_ms);
        if (given.contains ("position")) {
            for (int index = 1; index < given.at ("position").get<int> (); ++index)
                wait (connection (std::to_string (index), "earlier"));
            wait (connection (given));
        }
        if (given.contains ("reports")) {
            for (const auto &row : given.at ("reports"))
                state._reports.emplace (
                  node_of (connection (row)),
                  queue_state_t::report_state_t{
                    now.steady, row.at ("limit").get<int> () - row.at ("inFlight").get<int> ()
                                  - row.at ("issuedSinceReport").get<int> ()});
        }
        if (given.contains ("reportAgeMs"))
            state._reports.emplace (
              node_of (connection ("a")),
              queue_state_t::report_state_t{
                now.steady - std::chrono::milliseconds (given.at ("reportAgeMs").get<int> ()), 1});
        if (given.contains ("runningPermits")) {
            auto running = given.at ("runningPermits");
            if (running.is_number_integer ()) {
                const auto count = running.get<int> ();
                running = json_t::array ();
                for (int index = 0; index < count; ++index)
                    running.push_back ({{"node", "existing-node"}});
            }
            std::uint64_t id = 0;
            for (const auto &row : running) {
                state._permits.emplace (
                  ++id, queue_state_t::permit_t{node_of (connection (row)),
                                                {row.value ("ownerId", "existing-owner"),
                                                 row.value ("leaseGeneration", 1u)},
                                                row.value ("requestSeq", 1u)});
                if (row.contains ("permit"))
                    permit_names[row.at ("permit").get<std::string> ()] = id;
                last = id;
            }
        }
        if (given.contains ("waiters") && fixture.at ("name") == "delete-waiters")
            seed_ticket (wait (connection ("expired")), now.unix_ms + 1);
    }

    void run ()
    {
        for (const auto &step : fixture.at ("steps")) {
            const auto action = step.at ("action").get<std::string> ();
            const auto &expect = step.at ("expect");
            if (action == "enqueue") {
                const auto before = state._waiters.size ();
                const auto identity = connection (step.contains ("sessionRid") ? step : given);
                auto reply = state.enqueue (identity, now);
                if (expect.contains ("position"))
                    check (std::get<position_t> (reply.delivery.value).position
                           == expect.at ("position"));
                if (expect.contains ("newWaiters"))
                    check (state._waiters.size () - before == expect.at ("newWaiters"));
                if (expect.contains ("ticket") && !expect.at ("ticket").is_null ())
                    check (std::get<issued_ticket_t> (reply.delivery.value).ticket
                           == issued_ticket);
                if (expect.contains ("ticket") && expect.at ("ticket").is_null ())
                    check (std::holds_alternative<position_t> (reply.delivery.value));
            } else if (action == "issueTicket") {
                auto reply = apply_report ({.node = node_of (connection (given)), .limit = 1});
                check (reply.deliveries.size () == 1);
                issued_ticket = std::get<issued_ticket_t> (reply.deliveries.front ().value).ticket;
            } else if (action == "nodeReport") {
                node_report_t report{.node = node_of (connection (step)), .limit = 1};
                for (const auto &used : step.value (
                       "usedTickets", step.value ("usedTicketSessions", json_t::array ())))
                    report.used.push_back (session (used));
                for (const auto &ended : step.value ("endedSessions", json_t::array ()))
                    report.ended.push_back (session (ended));
                for (const auto &permit : step.value ("heldPermits", json_t::array ()))
                    report.held.push_back (permit_names.at (permit.get<std::string> ()));
                report.resolved_request_seq = step.value ("resolvedRequestSeq", 0u);
                const auto reply = apply_report (report, expect);
                if (expect.contains ("ticketSessions")) {
                    auto ticket_sessions = json_t::array ();
                    for (const auto &delivery : reply.deliveries) {
                        if (std::holds_alternative<issued_ticket_t> (delivery.value))
                            ticket_sessions.push_back (text (delivery.session.rid));
                    }
                    check (ticket_sessions == expect.at ("ticketSessions"));
                }
                if (expect.contains ("waiting")) {
                    json_t waiting = json_t::array ();
                    for (const auto &waiter : state._waiters)
                        waiting.push_back ({{"node", text (waiter.connection.node_rid)},
                                            {"sessionRid", text (waiter.connection.session_rid)}});
                    check (waiting == expect.at ("waiting"));
                }
            } else if (action == "calculateBudget") {
                const auto found = state._reports.find (node_of (connection ("a")));
                check (found != state._reports.end ());
                check (
                  std::min (static_cast<std::size_t> (found->second.budget), state.global_budget ())
                  == expect.at ("budget"));
            } else if (action == "issue") {
                if (given.contains ("invalidatedTickets"))
                    state._reports.emplace (node_of (state._waiters.front ().connection),
                                            queue_state_t::report_state_t{now.steady, 1});
                state.expire (now);
                state.issue (now);
                check (sessions (true) == expect.at ("ticketSessions"));
                if (expect.contains ("remaining"))
                    check (sessions (false) == expect.at ("remaining"));
            } else if (action == "admitPermit") {
                const auto id = state.admit ({connection (step),
                                              {step.at ("ownerId"), step.at ("leaseGeneration")},
                                              step.at ("epoch"),
                                              step.at ("requestSeq")},
                                             now);
                check ((id != 0) == expect.at ("granted"));
                if (id) {
                    last = id;
                    if (step.contains ("permit"))
                        permit_names[step.at ("permit").get<std::string> ()] = id;
                }
            } else if (action == "releasePermit") {
                if (const auto permit = state._permits.find (last); permit != state._permits.end ())
                    last_node = permit->second.node;
                state.release ({last_node, last});
            } else if (action == "ownerLeaseEnded") {
                state.owner_lease_ended ({step.at ("ownerId"), step.at ("leaseGeneration")});
            } else if (action == "nodeWaiters") {
                json_t order = json_t::array ();
                for (const auto &waiter : state._waiters)
                    if (text (waiter.connection.node_rid) == step.at ("node").get<std::string> ())
                        order.push_back (text (waiter.connection.session_rid));
                check (order == expect.at ("order"));
            } else if (action == "estimate") {
                check (queue_state_t::estimate_wait_ms (step.at ("position"),
                                                        step.at ("ticketsLast30Seconds"))
                       == expect.at ("estimatedWaitMs"));
            } else if (action == "ticketExpires") {
                ++now.unix_ms;
                state.expire (now);
            } else if (action == "reinitialize") {
                state = queue_state_t (signer, global_limit (given), expect.at ("epoch"));
                check (state._waiters.empty ());
            } else if (action == "verifyOldTicket") {
                const auto identity = connection ("a");
                const auto ticket = signer->issue (identity, step.at ("ticketEpoch"),
                                                   now.unix_ms + ticket_lifetime_ms);
                check (signer->verify (identity, ticket, state._epoch, now.unix_ms)
                       == expect.at ("accept"));
            } else {
                throw std::runtime_error ("Unknown admission queue action " + action);
            }
            if (expect.contains ("runningPermits"))
                check (state._permits.size () == expect.at ("runningPermits"));
            if (expect.contains ("waiters")) {
                auto waiters = sessions (false);
                for (auto &ticketed : sessions (true))
                    waiters.push_back (ticketed);
                check (waiters == expect.at ("waiters"));
            }
        }
    }

  private:
    void check (bool value) const
    {
        if (!value)
            throw std::runtime_error (fixture.at ("name").get<std::string> ());
    }

    report_reply_t apply_report (const node_report_t &report,
                                 const json_t &expect = json_t::object ())
    {
        const auto owners = state.accept_report (report, now);
        if (expect.contains ("ownerLeaseChecks"))
            check (owners.size () == expect.at ("ownerLeaseChecks"));
        const auto leases = given.value ("ownerLeases", json_t::object ());
        std::set<owner_t> ended;
        for (const auto &owner : owners) {
            if (leases.value (owner.id + ":" + std::to_string (owner.generation), "live")
                == "ended")
                ended.insert (owner);
        }
        return state.finish_report (report.node, now, ended);
    }

    queue_state_t::waiter_t &wait (connection_t identity)
    {
        auto found =
          std::ranges::find (state._waiters, identity, &queue_state_t::waiter_t::connection);
        if (found != state._waiters.end ())
            return *found;
        return state._waiters.emplace_back (
          queue_state_t::waiter_t{.connection = std::move (identity)});
    }

    void seed_ticket (queue_state_t::waiter_t &waiter, std::uint64_t expires)
    {
        waiter.expiration = expires;
        waiter.ticket = issued_ticket_t{signer->issue (waiter.connection, state._epoch, expires),
                                        static_cast<std::int32_t> (ticket_lifetime_ms)};
    }

    json_t sessions (bool ticketed) const
    {
        auto result = json_t::array ();
        for (const auto &waiter : state._waiters)
            if (waiter.ticket.has_value () == ticketed)
                result.push_back (text (waiter.connection.session_rid));
        return result;
    }

    const json_t &fixture;
    const json_t &given;
    std::shared_ptr<ticket_signer_t> signer;
    queue_state_t state;
    queue_time_t now;
    std::string issued_ticket;
    std::uint64_t last = 0;
    node_t last_node;
    std::map<std::string, std::uint64_t> permit_names;
};

} // namespace zlink::framework::runtime::admission_queue

int main (int argc, char *argv[])
{
    using namespace zlink::framework::runtime::admission_queue;
    try {
        const auto fixture =
          json_t::parse (std::ifstream (argc == 2 ? argv[1] : ZLINK_ADMISSION_QUEUE_PATH));
        const auto &scenarios = fixture.at ("queueScenarios");
        if (scenarios.size () < 20)
            throw std::runtime_error ("Expected twenty queue scenarios");
        for (const auto &scenario : scenarios) {
            queue_fixture_t (scenario).run ();
            std::cout << "PASS " << scenario.at ("name").get<std::string> () << '\n';
        }
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
