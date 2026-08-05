/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "player_actor.hpp"

namespace zlink::samples::tictactoe
{

struct player_actor_state_t
{
    std::string actor_id;
    std::string node_rid;
    unsigned long long generation = 1;
    bool destroy_after_entry_spot_join = false;
    bool disconnected = false;
    player_info_t player;
};

inline void to_json (nlohmann::json &json, const player_actor_state_t &value)
{
    json = nlohmann::json{{"actorId", value.actor_id},
                          {"nodeRid", value.node_rid},
                          {"generation", value.generation},
                          {"destroyAfterEntrySpotJoin", value.destroy_after_entry_spot_join},
                          {"disconnected", value.disconnected},
                          {"player", value.player}};
}

inline void from_json (const nlohmann::json &json, player_actor_state_t &value)
{
    value.actor_id = json.value ("actorId", std::string{});
    value.node_rid = json.value ("nodeRid", std::string{});
    value.generation = json.value ("generation", 1ull);
    value.destroy_after_entry_spot_join =
      json.value ("destroyAfterEntrySpotJoin", false);
    value.disconnected = json.value ("disconnected", false);
    value.player = json.value ("player", player_info_t{});
}

// --8<-- [start:doc-relocation-adapter]
class player_actor_relocation_adapter_t final
    : public actor_relocation_adapter_t<player_actor_t>
{
  public:
    task_t<std::vector<std::byte>>
    capture (player_actor_t &actor, std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (player_actor_state_t{
          {}, {}, 1, actor.destroy_after_entry_spot_join,
          actor.disconnected, actor.player});
        co_return std::vector<std::byte> (
          message.bytes ().begin (), message.bytes ().end ());
    }

    task_t<void>
    restore (player_actor_t &actor,
             std::vector<std::byte> payload,
             std::stop_token) override
    {
        const auto message = zlink::message_t::from (
          std::span<const std::byte> (payload.data (), payload.size ()));
        auto restored = message.parse_json<player_actor_state_t> ();
        actor.destroy_after_entry_spot_join = restored.destroy_after_entry_spot_join;
        actor.disconnected = restored.disconnected;
        actor.player = std::move (restored.player);
        co_return;
    }
};
// --8<-- [end:doc-relocation-adapter]

} // namespace zlink::samples::tictactoe
