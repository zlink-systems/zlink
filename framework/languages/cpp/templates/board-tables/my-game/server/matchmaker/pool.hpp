#pragma once
#include "domain.hpp"
#include "server/shared/contracts.hpp"
#include "server/shared/identity.hpp"
#include "server/shared/match_contracts.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Pool;
struct MatchTimer
{
    fx::task_t<void> handle (Pool &pool, const fx::timer_tick_t &) const;
};

class Pool final : public fx::instance_spot_t
{
    fx::instance_spot_context_t context_;
    fx::actor_client_t &actors_;
    MatchQueue queue_;
    fx::timer_t timer_;

    template <class T> fx::task_t<void> notify (const Ticket &ticket, const T &message)
    {
        co_await actors_.send (fx::actor_id_t (ticket.player), message).async ();
    }

    fx::task_t<void> apply (const MatchDecision &decision, std::string_view requester = {})
    {
        const auto &assignment = decision.assignment;
        if (decision.committed) {
            const CommitRoom commit{assignment.id};
            co_await context_.send_to_spot (assignment.id, commit).async ();
            co_return;
        }
        const WithdrawRoom withdraw{assignment.id};
        co_await context_.send_to_spot (MatchNames::provisioner_key, withdraw)
          .instance_spot (MatchNames::provisioner)
          .in_mesh (Names::mesh)
          .async ();
        for (std::size_t index = 0; index < assignment.tickets.size (); ++index) {
            const auto &ticket = assignment.tickets[index];
            if (decision.reasons[index] == MatchEnd::requeue) {
                co_await notify (ticket, withdraw);
            } else if (ticket.id != requester) {
                const MatchCancelled cancelled{
                  ticket.id, decision.reasons[index] == MatchEnd::expired ? MatchNames::expired
                                                                          : MatchNames::cancelled};
                co_await notify (ticket, cancelled);
            }
        }
    }

  public:
    Pool (fx::instance_spot_context_t context, fx::actor_client_t &actors) :
        context_ (std::move (context)), actors_ (actors)
    {
    }
    fx::instance_spot_context_t &context () noexcept override { return context_; }
    const fx::instance_spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_handler<&Pool::enqueue> ()
          .add_handler<&Pool::cancel> ()
          .add_handler<&Pool::report> ()
          .add_handler<&Pool::prepared> ();
    }
    fx::task_t<void> on_initialize () override
    {
        timer_ = context_.add_timer<MatchTimer> ("match", std::chrono::milliseconds (500));
        co_return;
    }

    fx::task_t<void> prepared (const RoomPrepared &reply)
    {
        auto *assignment = queue_.awaiting_preparation (reply.assignmentId, MatchClock::now ());
        if (!assignment)
            co_return;
        if (!reply.ok) {
            const auto decision = queue_.prepare_failed (reply.assignmentId);
            co_await apply (decision);
            co_return;
        }
        assignment->prepared = true;
        for (const auto &ticket : assignment->tickets) {
            const MatchAssigned assigned{ticket.id, assignment->id, assignment->id};
            co_await notify (ticket, assigned);
        }
    }

    fx::task_t<MatchQueued> enqueue (const MatchRequest &request)
    {
        if (request.requestKey.empty ())
            throw std::invalid_argument ("A match request requires a request key");
        const auto id = queue_.enqueue (request.playerId, new_id ("ticket-"), MatchClock::now ());
        co_await advance ();
        co_return MatchQueued{id};
    }

    fx::task_t<MatchCancelled> cancel (const CancelTicket &request)
    {
        if (request.reason != MatchNames::cancelled && request.reason != MatchNames::disconnected)
            throw std::invalid_argument ("Unknown cancellation reason");
        const auto result =
          queue_.cancel (request.ticketId, request.reason == MatchNames::cancelled);
        if (result.assignment)
            co_await apply (*result.assignment, request.ticketId);
        const MatchCancelled cancelled{request.ticketId, request.reason};
        if (result.waiting && request.reason == MatchNames::disconnected)
            co_await notify (*result.waiting, cancelled);
        if (!result.waiting && !result.assignment && request.reason == MatchNames::cancelled)
            throw fx::framework_exception_t (fx::framework_error_kind_t::rejected,
                                             "Ticket is no longer active");
        co_return cancelled;
    }

    fx::task_t<void> report (const EnterReport &report)
    {
        if (const auto decision =
              queue_.report (report.assignmentId, report.ticketId, report.ok, MatchClock::now ()))
            co_await apply (*decision);
    }

    fx::task_t<void> advance ()
    {
        const auto now = MatchClock::now ();
        for (const auto &decision : queue_.timed_out (now))
            co_await apply (decision);
        for (const auto &ticket : queue_.expired (now)) {
            const MatchCancelled expired{ticket.id, MatchNames::expired};
            co_await notify (ticket, expired);
        }
        while (const auto assignment = queue_.reserve (
                 [] { return new_id (MatchNames::room_prefix); }, MatchClock::now ())) {
            const PrepareRoom prepare{
              assignment->id, {assignment->tickets[0].player, assignment->tickets[1].player}};
            co_await context_.send_to_spot (MatchNames::provisioner_key, prepare)
              .instance_spot (MatchNames::provisioner)
              .in_mesh (Names::mesh)
              .async ();
        }
    }
};

inline fx::task_t<void> MatchTimer::handle (Pool &pool, const fx::timer_tick_t &) const
{
    co_await pool.advance ();
}
}
