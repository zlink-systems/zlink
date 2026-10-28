#pragma once
#include "domain.hpp"
#include <zlink/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
struct ModeQueue
{
    std::string mode;
    std::vector<QueueRequest> requests;
};

struct MatchmakingState
{
    std::vector<ModeQueue> waiting;
    std::int64_t roomSequence{};

    void enqueue (const QueueRequest &request)
    {
        const auto nonblank = std::any_of (request.mode.begin (), request.mode.end (), [] (char c) {
            return !std::isspace (static_cast<unsigned char> (c));
        });
        if (!nonblank)
            throw std::invalid_argument ("Mode required");
        for (const auto &entry : waiting) {
            for (const auto &player : entry.requests)
                if (player.playerId == request.playerId)
                    return;
        }
        auto found = std::find_if (waiting.begin (), waiting.end (), [&] (const auto &entry) {
            return entry.mode == request.mode;
        });
        if (found == waiting.end ())
            waiting.push_back ({request.mode, {request}});
        else
            found->requests.push_back (request);
    }
    std::vector<QueueRequest> pair () const
    {
        for (const auto &entry : waiting) {
            if (entry.requests.size () >= Game::players_per_room)
                return {entry.requests.begin (), entry.requests.begin () + Game::players_per_room};
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
            return entry.mode == players.front ().mode;
        });
        if (queue == waiting.end () || queue->requests.size () < Game::players_per_room)
            throw std::invalid_argument ("Matchmaking pair is not queued");
        queue->requests.erase (queue->requests.begin (), queue->requests.begin () + Game::players_per_room);
        if (queue->requests.empty ())
            waiting.erase (queue);
    }
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::ModeQueue>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("mode", &::MyGame::ModeQueue::mode),
      zlink::json::field ("requests", &::MyGame::ModeQueue::requests)
    };
};

template <> struct dto_traits<::MyGame::MatchmakingState>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("waiting", &::MyGame::MatchmakingState::waiting),
      zlink::json::field ("roomSequence", &::MyGame::MatchmakingState::roomSequence)
    };
};
} // namespace zlink::json
