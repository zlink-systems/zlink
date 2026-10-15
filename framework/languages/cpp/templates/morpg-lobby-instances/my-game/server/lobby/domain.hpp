#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <set>
namespace MyGame
{
struct PartyState
{
    std::string partyId;
    std::vector<std::string> members;
    std::set<std::string> ready;
    bool join (const std::string &id, const PartyJoin &request)
    {
        if (!nonblank (request.partyId) || std::ranges::find (members, id) != members.end ()
            || members.size () >= Names::maximum_members
            || (request.create ? !members.empty ()
                               : partyId != request.partyId || members.empty ()))
            return false;
        partyId = request.partyId;
        members.push_back (id);
        return true;
    }
    void leave (const std::string &id)
    {
        std::erase (members, id);
        ready.erase (id);
    }
    PartyView view () const
    {
        PartyView result{partyId, members, {}};
        for (const auto &id : members)
            result.ready.push_back (ready.contains (id));
        return result;
    }
    std::string entry_rejection (const std::string &id) const
    {
        if (members.empty () || members.front () != id)
            return Rejection::not_leader;
        for (const auto &member : members)
            if (!ready.contains (member))
                return Rejection::not_ready;
        return {};
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyState, partyId, members, ready)
};
}
