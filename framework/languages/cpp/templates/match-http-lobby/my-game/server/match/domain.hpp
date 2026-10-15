#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
namespace MyGame
{
struct Combatant
{
    PlayerView view;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Combatant, view, sequence)
};
class RoomState
{
    Combatant &player (const std::string &id)
    {
        const auto found = std::find_if (players.begin (), players.end (),
                                         [&] (const auto &p) { return p.view.playerId == id; });
        if (found == players.end ())
            throw std::invalid_argument ("Unknown room player");
        return *found;
    }
    std::string validate (Combatant &p, std::int64_t sequence)
    {
        if (sequence <= p.sequence)
            return Rejection::stale;
        p.sequence = sequence;
        return winner ? Rejection::finished : "";
    }

  public:
    std::vector<Combatant> players;
    std::optional<std::string> winner;
    bool admit (const std::string &id)
    {
        for (const auto &p : players)
            if (p.view.playerId == id)
                return true;
        if (players.size () >= Game::players_per_room)
            return false;
        players.push_back (Combatant{PlayerView{id}});
        return true;
    }
    std::string move (const std::string &id, const MoveRequest &request)
    {
        auto &p = player (id);
        if (const auto error = validate (p, request.sequence); !error.empty ())
            return error;
        if (!std::isfinite (request.x) || !std::isfinite (request.y))
            return Rejection::invalid_move;
        p.view.x = request.x;
        p.view.y = request.y;
        return {};
    }
    std::string shoot (const std::string &id, const ShootRequest &request)
    {
        auto &p = player (id);
        if (const auto error = validate (p, request.sequence); !error.empty ())
            return error;
        const auto target = std::find_if (players.begin (), players.end (), [&] (const auto &p) {
            return p.view.playerId == request.targetId && p.view.playerId != id && p.view.hp > 0;
        });
        if (target == players.end ())
            return Rejection::no_target;
        const auto &a = p.view;
        auto &b = target->view;
        const auto dx = b.x - a.x, dy = b.y - a.y;
        if (dx * dx + dy * dy > Game::range * Game::range)
            return Rejection::range;
        bool blocked;
        if (dx == 0)
            blocked = a.x == Game::wall_x && std::max (a.y, b.y) >= Game::wall_min_y
                      && std::min (a.y, b.y) <= Game::wall_max_y;
        else {
            const auto t = (Game::wall_x - a.x) / dx, y = a.y + t * dy;
            blocked = t >= 0 && t <= 1 && y >= Game::wall_min_y && y <= Game::wall_max_y;
        }
        if (blocked)
            return Rejection::wall;
        b.hp = std::max (0, b.hp - Game::damage);
        if (b.hp == 0)
            winner = id;
        return {};
    }
    std::vector<PlayerView> view () const
    {
        std::vector<PlayerView> result;
        result.reserve (players.size ());
        for (const auto &p : players)
            result.push_back (p.view);
        return result;
    }
    friend void to_json (nlohmann::json &j, const RoomState &state)
    {
        j = {{"players", state.players},
             {"winner", state.winner ? nlohmann::json (*state.winner) : nlohmann::json (nullptr)}};
    }
    friend void from_json (const nlohmann::json &j, RoomState &state)
    {
        state.players = j.at ("players").get<std::vector<Combatant>> ();
        state.winner.reset ();
        if (!j.at ("winner").is_null ())
            state.winner = j.at ("winner").get<std::string> ();
    }
};
}
