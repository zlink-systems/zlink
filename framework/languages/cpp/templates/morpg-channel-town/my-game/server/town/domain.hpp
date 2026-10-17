#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/domain.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace MyGame
{
struct TownState
{
    std::set<std::string> players;
    std::map<std::string, std::vector<std::string>> parties;
    bool party (const std::string &id, const std::string &party_id, bool create)
    {
        const auto existing = parties.find (party_id);
        if (blank (party_id) || !players.contains (id)
            || std::any_of (parties.begin (), parties.end (),
                            [&] (const auto &item) {
                                return std::find (item.second.begin (), item.second.end (), id)
                                       != item.second.end ();
                            })
            || (create ? existing != parties.end ()
                       : existing == parties.end ()
                           || existing->second.size () >= Names::maximum_party_members))
            return false;
        if (create)
            parties.emplace (party_id, std::vector<std::string>{id});
        else
            existing->second.push_back (id);
        return true;
    }
    void leave (const std::string &id)
    {
        players.erase (id);
        for (auto it = parties.begin (); it != parties.end ();) {
            std::erase (it->second, id);
            if (it->second.empty ())
                it = parties.erase (it);
            else
                ++it;
        }
    }
    auto led_by (const std::string &id) const
    {
        return std::find_if (parties.begin (), parties.end (),
                             [&] (const auto &item) { return item.second.front () == id; });
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (TownState, players, parties)
};
}
