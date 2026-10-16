#pragma once
#include "server/shared/hosting.hpp"
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
namespace MyGame
{
using MatchCompletion = fx::task_completion_source_t<MatchResponse>;
// One entry into the lobby: either a pending request or a pairing with the earlier request.
struct MatchTicket
{
    std::shared_ptr<MatchCompletion> pending;
    std::string room;
    std::shared_ptr<MatchCompletion> partner;
};
// HTTP handlers run concurrently. This lock owns only the application lobby queue.
class Matchmaker final
{
    struct Waiting
    {
        std::string player;
        std::shared_ptr<MatchCompletion> completion;
    };
    std::mutex gate_;
    std::map<std::string, Waiting> queues_;
    std::set<std::string> participants_;
    std::uint64_t sequence_ = 0;

  public:
    MatchTicket enter (const MatchRequest &request)
    {
        if (request.playerId.find_first_not_of (" \t\r\n") == std::string::npos
            || request.mode.find_first_not_of (" \t\r\n") == std::string::npos)
            throw fx::framework_exception_t (fx::framework_error_kind_t::invalid_operation,
                                             "Nonempty player and mode required");
        const std::lock_guard lock (gate_);
        if (participants_.contains (request.playerId))
            throw fx::framework_exception_t (fx::framework_error_kind_t::invalid_operation,
                                             "Player already has a pending match request");
        const auto found = queues_.find (request.mode);
        if (found == queues_.end ()) {
            auto completion = std::make_shared<MatchCompletion> ();
            participants_.insert (request.playerId);
            queues_.emplace (request.mode, Waiting{request.playerId, completion});
            return {completion, {}, {}};
        }
        if (sequence_ == std::numeric_limits<std::uint64_t>::max ())
            throw std::overflow_error ("Room sequence exhausted");
        auto partner = std::move (found->second.completion);
        participants_.erase (found->second.player);
        queues_.erase (found);
        return {
          {}, std::string (Names::room_prefix) + std::to_string (++sequence_), std::move (partner)};
    }
};
}
