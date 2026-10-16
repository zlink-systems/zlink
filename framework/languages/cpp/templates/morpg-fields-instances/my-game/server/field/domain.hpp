#pragma once
#include "server/shared/player-state.hpp"
#include <map>
#include <algorithm>

namespace MyGame
{
// Visibility is an application projection; PlayerState owns movement.
struct FieldState
{
    std::map<std::string, EnterField> visiblePlayers, arrivals;
    std::map<std::string, std::vector<std::string>> parties;
    bool party (const std::string &id, const std::string &partyId, bool create)
    {
        if (!Names::valid_id (partyId) || !visiblePlayers.contains (id))
            return false;
        for (const auto &[key, members] : parties)
            if (std::find (members.begin (), members.end (), id) != members.end ())
                return false;
        auto found = parties.find (partyId);
        if (create) {
            if (found != parties.end ())
                return false;
            parties.emplace (partyId, std::vector<std::string>{id});
        } else {
            if (found == parties.end () || found->second.size () >= Names::maximum_party_members)
                return false;
            found->second.push_back (id);
        }
        return true;
    }
    void leave (const std::string &id)
    {
        visiblePlayers.erase (id);
        for (auto it = parties.begin (); it != parties.end (); ++it) {
            auto &members = it->second;
            auto member = std::find (members.begin (), members.end (), id);
            if (member == members.end ())
                continue;
            members.erase (member);
            if (members.empty ())
                parties.erase (it);
            break;
        }
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldState, visiblePlayers, arrivals, parties)
};
}
