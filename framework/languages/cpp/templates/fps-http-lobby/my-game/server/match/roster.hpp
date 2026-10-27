#pragma once
#include "domain.hpp"
#include "server/shared/http_lobby_contracts.hpp"
#include <set>

namespace MyGame
{
struct RosterSnapshot
{
    std::optional<std::string> matchId{};
    std::vector<std::string> members{};
};
inline void to_json (nlohmann::json &j, const RosterSnapshot &value)
{
    j = nlohmann::json::object ();
    j["matchId"] = detail::to_nullable_json (value.matchId);
    j["members"] = value.members;
}
inline void from_json (const nlohmann::json &j, RosterSnapshot &value)
{
    detail::from_nullable_json (j.at ("matchId"), value.matchId);
    j.at ("members").get_to (value.members);
}

struct HttpRoomSnapshot
{
    RoomSnapshot game{};
    RosterSnapshot roster{};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (HttpRoomSnapshot, game, roster)
};

class RoomRoster
{
  public:
    RoomPrepared prepare (std::string_view roomId, const PrepareRoom &request)
    {
        if (request.matchId != roomId || roomId.empty ()
            || request.members.size () != Game::players_per_room
            || std::any_of (request.members.begin (), request.members.end (), [] (const auto &id) {
                   return std::all_of (id.begin (), id.end (), [] (unsigned char c) {
                       return std::isspace (c) != 0;
                   });
               })
            || std::set<std::string> (request.members.begin (), request.members.end ()).size ()
                 != request.members.size ())
            throw std::invalid_argument ("Invalid room roster preparation.");

        const std::set<std::string> requested (request.members.begin (), request.members.end ());
        if (matchId_) {
            if (*matchId_ != request.matchId || members_ != requested)
                throw std::logic_error ("A prepared Room roster cannot be replaced.");
            return RoomPrepared{*matchId_};
        }
        matchId_ = request.matchId;
        members_ = requested;
        return RoomPrepared{*matchId_};
    }

    bool can_admit (std::string_view playerId) const
    {
        return matchId_.has_value () && members_.contains (std::string (playerId));
    }

    RosterSnapshot capture () const
    {
        return {matchId_, {members_.begin (), members_.end ()}};
    }

    void restore (const RosterSnapshot &snapshot)
    {
        if (matchId_)
            throw std::logic_error ("Cannot restore an already prepared Room roster.");
        if (snapshot.matchId)
            prepare (*snapshot.matchId, PrepareRoom{*snapshot.matchId, snapshot.members});
        else if (!snapshot.members.empty ())
            throw std::invalid_argument ("Unprepared Room snapshot has members.");
    }

  private:
    std::optional<std::string> matchId_{};
    std::set<std::string> members_{};
};
}
