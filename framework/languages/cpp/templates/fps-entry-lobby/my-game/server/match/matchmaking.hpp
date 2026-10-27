#pragma once
#include "domain.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace MyGame
{
struct MatchmakingState
{
    std::vector<std::pair<std::string, std::vector<QueueRequest>>> waiting;
    std::int64_t roomSequence{};

    void enqueue (const QueueRequest &request)
    {
        const auto nonblank = std::any_of (request.mode.begin (), request.mode.end (), [] (char c) {
            return !std::isspace (static_cast<unsigned char> (c));
        });
        if (!nonblank)
            throw std::invalid_argument ("Mode required");
        for (const auto &[mode, queue] : waiting) {
            (void) mode;
            for (const auto &player : queue)
                if (player.playerId == request.playerId)
                    return;
        }
        auto found = std::find_if (waiting.begin (), waiting.end (), [&] (const auto &entry) {
            return entry.first == request.mode;
        });
        if (found == waiting.end ())
            waiting.push_back ({request.mode, {request}});
        else
            found->second.push_back (request);
    }
    std::vector<QueueRequest> pair () const
    {
        for (const auto &[mode, queue] : waiting) {
            (void) mode;
            if (queue.size () >= Game::players_per_room)
                return {queue.begin (), queue.begin () + Game::players_per_room};
        }
        return {};
    }
    std::string allocate_room ()
    {
        if (roomSequence == std::numeric_limits<std::int64_t>::max ())
            throw std::overflow_error ("Room sequence exhausted");
        return Names::room_prefix + std::to_string (++roomSequence);
    }
    void remove (const std::vector<QueueRequest> &players)
    {
        if (players.empty ())
            return;
        auto queue = std::find_if (waiting.begin (), waiting.end (), [&] (const auto &entry) {
            return entry.first == players.front ().mode;
        });
        if (queue == waiting.end () || queue->second.size () < Game::players_per_room)
            throw std::invalid_argument ("Matchmaking pair is not queued");
        queue->second.erase (queue->second.begin (), queue->second.begin () + Game::players_per_room);
        if (queue->second.empty ())
            waiting.erase (queue);
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchmakingState, waiting, roomSequence)
};
}
