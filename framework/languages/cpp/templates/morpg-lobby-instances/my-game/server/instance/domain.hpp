#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <limits>
#include <set>
namespace MyGame {
struct DungeonState
{
  std::string partyId;
  std::vector<std::string> members;
  int stage = 0;
  void prepare(const PrepareDungeon& request, std::string_view spot)
  {
    const std::set<std::string> unique(request.members.begin(),
                                       request.members.end());
    if (!partyId.empty() || !nonblank(request.partyId) ||
        request.members.empty() ||
        request.members.size() > Names::maximum_members ||
        unique.size() != request.members.size() ||
        std::ranges::any_of(request.members,
                            [](const auto& id) { return !nonblank(id); }) ||
        Names::dungeon_prefix + request.partyId != spot)
      throw std::invalid_argument("Invalid dungeon party preparation");
    partyId = request.partyId;
    members = request.members;
  }
  bool admits(std::string_view id,
              const JoinDungeon& request,
              std::string_view spot) const
  {
    return request.dungeonId == spot && request.partyId == partyId &&
           std::ranges::find(members, id) != members.end();
  }
  void advance()
  {
    if (stage == std::numeric_limits<int>::max())
      throw std::overflow_error("Dungeon stage exhausted");
    ++stage;
  }
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(DungeonState, partyId, members, stage)
};
}
