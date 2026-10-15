#pragma once
#include "server/shared/contracts.hpp"
#include <map>
namespace MyGame
{
struct Position
{
    int x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Position, x, sequence)
};
class Positions
{
    std::map<std::string, Position> players_;

  public:
    bool admit (const std::string &id, int capacity)
    {
        if (players_.contains (id))
            return true;
        if (players_.size () >= static_cast<std::size_t> (capacity))
            return false;
        players_.emplace (id, Position{});
        return true;
    }
    void remove (const std::string &id) { players_.erase (id); }
    void move (const std::string &id, const MoveRequest &request)
    {
        auto &position = players_.at (id);
        if (request.sequence <= position.sequence)
            throw std::invalid_argument ("Move sequence must increase");
        position = Position{request.x, request.sequence};
    }
    std::vector<PlayerPosition> view () const
    {
        std::vector<PlayerPosition> result;
        result.reserve (players_.size ());
        for (const auto &[id, position] : players_)
            result.push_back ({id, position.x});
        return result;
    }
    const auto &players () const { return players_; }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Positions, players_)
};
}
