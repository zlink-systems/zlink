#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace MyGame
{
struct MatchmakingState
{
    std::vector<std::pair<std::string, std::vector<QueueRequest>>> waiting;
    std::int64_t roomSequence = 0;
    void enqueue (const QueueRequest &request)
    {
        if (request.mode.find_first_not_of (" \t\r\n") == std::string::npos)
            throw std::invalid_argument ("Mode required");
        for (const auto &[mode, queue] : waiting)
            for (const auto &p : queue)
                if (p.playerId == request.playerId)
                    return;
        auto found = std::find_if (waiting.begin (), waiting.end (),
                                   [&] (const auto &q) { return q.first == request.mode; });
        if (found == waiting.end ())
            waiting.push_back ({request.mode, {request}});
        else
            found->second.push_back (request);
    }
    std::vector<QueueRequest> pair () const
    {
        for (const auto &[mode, queue] : waiting)
            if (queue.size () >= Game::players_per_room)
                return {queue.begin (), queue.begin () + Game::players_per_room};
        return {};
    }
    std::string allocate_room ()
    {
        if (roomSequence == std::numeric_limits<std::int64_t>::max ())
            throw std::overflow_error ("Room sequence exhausted");
        return Names::room_prefix + std::to_string (++roomSequence);
    }
    void remove (const std::vector<QueueRequest> &pair)
    {
        auto q = std::find_if (waiting.begin (), waiting.end (), [&] (const auto &entry) {
            return entry.first == pair.front ().mode;
        });
        q->second.erase (q->second.begin (), q->second.begin () + Game::players_per_room);
        if (q->second.empty ())
            waiting.erase (q);
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchmakingState, waiting, roomSequence)
};
struct Combatant
{
    std::string playerId;
    double x = 0, y = 0;
    int hp = Game::initial_hp;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Combatant, playerId, x, y, hp, sequence)
};
struct RoomState
{
    std::vector<Combatant> players;
    std::string winner;
    bool admit (const std::string &id)
    {
        if (std::any_of (players.begin (), players.end (),
                         [&] (const auto &p) { return p.playerId == id; }))
            return true;
        if (players.size () >= Game::players_per_room)
            return false;
        players.push_back (Combatant{id});
        return true;
    }
    Combatant &player (std::string_view id)
    {
        for (auto &p : players)
            if (p.playerId == id)
                return p;
        throw std::invalid_argument ("Unknown room player");
    }
    const char *validate (Combatant &p, std::int64_t sequence)
    {
        if (sequence <= p.sequence)
            return Rejection::stale;
        p.sequence = sequence;
        return winner.empty () ? nullptr : Rejection::finished;
    }
    const char *move (std::string_view id, const MoveRequest &request)
    {
        auto &p = player (id);
        if (auto reason = validate (p, request.sequence))
            return reason;
        if (!std::isfinite (request.x) || !std::isfinite (request.y))
            return Rejection::invalid_move;
        p.x = request.x;
        p.y = request.y;
        return nullptr;
    }
    const char *shoot (std::string_view id, const ShootRequest &request)
    {
        auto &p = player (id);
        if (auto reason = validate (p, request.sequence))
            return reason;
        auto target = std::find_if (players.begin (), players.end (), [&] (const auto &t) {
            return t.playerId == request.targetId && t.playerId != id && t.hp > 0;
        });
        if (target == players.end ())
            return Rejection::no_target;
        const double dx = target->x - p.x, dy = target->y - p.y;
        if (dx * dx + dy * dy > Game::shot_range * Game::shot_range)
            return Rejection::out_of_range;
        bool blocked;
        if (dx == 0)
            blocked = p.x == Game::wall_x && std::max (p.y, target->y) >= Game::wall_min_y
                      && std::min (p.y, target->y) <= Game::wall_max_y;
        else {
            const double t = (Game::wall_x - p.x) / dx, y = p.y + t * dy;
            blocked = t >= 0 && t <= 1 && y >= Game::wall_min_y && y <= Game::wall_max_y;
        }
        if (blocked)
            return Rejection::blocked;
        target->hp = std::max (0, target->hp - Game::shot_damage);
        if (target->hp == 0)
            winner = std::string (id);
        return nullptr;
    }
    std::vector<PlayerView> view () const
    {
        std::vector<PlayerView> result;
        result.reserve (players.size ());
        for (const auto &p : players)
            result.push_back ({p.playerId, p.x, p.y, p.hp});
        return result;
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomState, players, winner)
};
}
