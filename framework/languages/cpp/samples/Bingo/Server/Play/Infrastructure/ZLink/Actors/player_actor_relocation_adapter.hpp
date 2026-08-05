/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "player_actor.hpp"

namespace zlink::samples::bingo
{

struct player_actor_state_t
{
    std::string display_name;
    bool destroy_after_entry_spot_join = false;
    bool disconnected = false;
};

inline void to_json (nlohmann::json &json, const player_actor_state_t &value)
{
    json = {{"displayName", value.display_name},
            {"destroyAfterEntrySpotJoin", value.destroy_after_entry_spot_join},
            {"disconnected", value.disconnected}};
}

inline void from_json (const nlohmann::json &json, player_actor_state_t &value)
{
    value.display_name = json.value ("displayName", std::string{});
    value.destroy_after_entry_spot_join =
      json.value ("destroyAfterEntrySpotJoin", false);
    value.disconnected = json.value ("disconnected", false);
}

class player_actor_relocation_adapter_t final
    : public actor_relocation_adapter_t<player_actor_t>
{
  public:
    task_t<std::vector<std::byte>>
    capture (player_actor_t &actor, std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (player_actor_state_t{
          actor.display_name, actor.destroy_after_entry_spot_join,
          actor.disconnected});
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
        actor.display_name = std::move (restored.display_name);
        actor.destroy_after_entry_spot_join = restored.destroy_after_entry_spot_join;
        actor.disconnected = restored.disconnected;
        co_return;
    }
};

} // namespace zlink::samples::bingo
