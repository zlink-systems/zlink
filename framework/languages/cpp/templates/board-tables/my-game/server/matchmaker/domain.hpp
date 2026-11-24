#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MyGame
{
using MatchClock = std::chrono::steady_clock;

struct Ticket
{
    std::string player;
    std::string id;
    MatchClock::time_point expires;
};

struct Assignment
{
    std::string id;
    std::array<Ticket, 2> tickets;
    MatchClock::time_point deadline;
    std::array<bool, 2> entered{};
};

struct MatchRule
{
    static std::optional<std::array<Ticket, 2>> select (std::deque<Ticket> &waiting)
    {
        if (waiting.size () < 2)
            return {};
        std::array<Ticket, 2> pair;
        for (auto &ticket : pair) {
            ticket = std::move (waiting.front ());
            waiting.pop_front ();
        }
        return pair;
    }
};

enum class MatchEnd
{
    requeue,
    cancelled,
    expired
};

struct MatchDecision
{
    Assignment assignment;
    std::array<MatchEnd, 2> reasons{};
    bool committed;
};

struct CancelDecision
{
    std::optional<Ticket> waiting;
    std::optional<MatchDecision> assignment;
};

class MatchQueue
{
    std::deque<Ticket> waiting_;
    std::map<std::string, Assignment> reserved_;

    MatchDecision withdraw (decltype (reserved_)::iterator active, std::array<MatchEnd, 2> reasons)
    {
        auto assignment = std::move (active->second);
        reserved_.erase (active);
        for (std::size_t index = assignment.tickets.size (); index-- > 0;)
            if (reasons[index] == MatchEnd::requeue)
                waiting_.push_front (assignment.tickets[index]);
        return {std::move (assignment), std::move (reasons), false};
    }

  public:
    std::string enqueue (std::string player, std::string id, MatchClock::time_point now)
    {
        for (const auto &ticket : waiting_)
            if (ticket.player == player)
                return ticket.id;
        for (const auto &[_, assignment] : reserved_)
            for (const auto &ticket : assignment.tickets)
                if (ticket.player == player)
                    return ticket.id;
        waiting_.push_back ({std::move (player), id, now + std::chrono::seconds (120)});
        return id;
    }

    std::optional<Assignment> reserve (std::string room, MatchClock::time_point now)
    {
        auto tickets = MatchRule::select (waiting_);
        if (!tickets)
            return {};
        Assignment assignment{std::move (room), std::move (*tickets),
                              now + std::chrono::seconds (10)};
        reserved_.emplace (assignment.id, assignment);
        return assignment;
    }

    std::optional<MatchDecision> report (const std::string &room,
                                         std::string_view ticket,
                                         bool accepted,
                                         MatchClock::time_point now)
    {
        auto active = reserved_.find (room);
        if (active == reserved_.end () || now >= active->second.deadline)
            return {};
        auto &assignment = active->second;
        for (std::size_t index = 0; index < assignment.tickets.size (); ++index) {
            if (assignment.tickets[index].id != ticket || assignment.entered[index])
                continue;
            if (!accepted) {
                std::array<MatchEnd, 2> reasons{};
                reasons[index] = MatchEnd::cancelled;
                return withdraw (active, std::move (reasons));
            }
            assignment.entered[index] = true;
            if (std::ranges::all_of (assignment.entered, [] (bool entered) { return entered; })) {
                MatchDecision decision{std::move (assignment), {}, true};
                reserved_.erase (active);
                return decision;
            }
        }
        return {};
    }

    std::vector<MatchDecision> timed_out (MatchClock::time_point now)
    {
        std::vector<MatchDecision> decisions;
        for (auto active = reserved_.begin (); active != reserved_.end ();) {
            auto current = active++;
            if (now < current->second.deadline)
                continue;
            std::array<MatchEnd, 2> reasons{};
            for (std::size_t index = 0; index < reasons.size (); ++index)
                if (!current->second.entered[index])
                    reasons[index] = MatchEnd::expired;
            decisions.push_back (withdraw (current, std::move (reasons)));
        }
        return decisions;
    }

    std::vector<Ticket> expired (MatchClock::time_point now)
    {
        std::vector<Ticket> expired;
        std::erase_if (waiting_, [&] (const Ticket &ticket) {
            if (now < ticket.expires)
                return false;
            expired.push_back (ticket);
            return true;
        });
        return expired;
    }

    CancelDecision cancel (std::string_view ticket_id, bool allow_reserved)
    {
        auto waiting = std::ranges::find (waiting_, ticket_id, &Ticket::id);
        if (waiting != waiting_.end ()) {
            auto ticket = std::move (*waiting);
            waiting_.erase (waiting);
            return {std::move (ticket), {}};
        }
        if (!allow_reserved)
            return {};
        for (auto active = reserved_.begin (); active != reserved_.end (); ++active) {
            for (std::size_t index = 0; index < active->second.tickets.size (); ++index) {
                if (active->second.tickets[index].id != ticket_id)
                    continue;
                std::array<MatchEnd, 2> reasons{};
                reasons[index] = MatchEnd::cancelled;
                return {{}, withdraw (active, std::move (reasons))};
            }
        }
        return {};
    }

    bool has_pair () const { return waiting_.size () >= 2; }

    MatchDecision prepare_failed (const std::string &room)
    {
        return withdraw (reserved_.find (room), {});
    }
};
}
