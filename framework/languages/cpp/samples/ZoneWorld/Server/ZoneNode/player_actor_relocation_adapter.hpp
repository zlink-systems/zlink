/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace zlink::samples::zoneworld
{

namespace fw = zlink::framework;

class player_actor_t final : public fw::actor_t
{
  public:
    explicit player_actor_t (fw::actor_context_t context) :
        _context (std::move (context))
    {
        player_id = std::string (
          _context.actor_ref ().actor_id ().value ());
    }

    fw::actor_context_t &context () noexcept override
    {
        return _context;
    }

    const fw::actor_context_t &context () const noexcept override
    {
        return _context;
    }

    fw::task_t<void> on_join_completed (
      const fw::actor_join_completion_t &completion) override
    {
        if (std::holds_alternative<
              fw::actor_join_accepted_t> (completion)) {
            std::cerr << "zoneworld-join-accepted player="
                      << player_id << " zone=" << zone_id << '\n';
            initial_entry = false;
        }
        else if (const auto *failed =
                   std::get_if<fw::actor_join_failed_t> (
                     &completion)) {
            std::cerr << "zoneworld-join-failed player="
                      << player_id << " kind="
                      << static_cast<int> (failed->error_kind)
                      << '\n';
        }
        else if (const auto *rejected =
                   std::get_if<fw::actor_join_rejected_t> (
                     &completion)) {
            std::cerr << "zoneworld-join-rejected player="
                      << player_id << '\n';
            auto reason =
              std::string (reject_reason_t::zone_maintenance);
            if (rejected->reply) {
                reason = rejected->reply
                           ->decode<enter_zone_res_t> ()
                           .error.value_or (reason);
            }
            if (!is_bot) {
                _context.bound_session ()
                  .send (move_rejected_notify_t{
                    reason, x, y})
                  .submit ();
            }
        }
        co_return;
    }

    std::string player_id;
    int x = 25;
    int y = 25;
    std::string zone_id = "zone-nw";
    bool is_bot = false;
    int dir_x = 0;
    int dir_y = 0;
    bool initial_entry = true;

  private:
    fw::actor_context_t _context;
};

class player_actor_factory_t final
    : public fw::actor_factory_t<player_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<player_actor_t>> create (
      fw::actor_context_t context,
      std::stop_token) override
    {
        co_return std::make_shared<player_actor_t> (
          std::move (context));
    }
};

struct player_state_t
{
    int x = 25;
    int y = 25;
    std::string zone_id = "zone-nw";
    bool is_bot = false;
    int dir_x = 0;
    int dir_y = 0;
    bool initial_entry = true;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (
  player_state_t, x, y, zone_id, is_bot, dir_x, dir_y,
  initial_entry)

class player_relocation_adapter_t final
    : public fw::actor_relocation_adapter_t<player_actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (
      player_actor_t &actor,
      std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (
          player_state_t{
            actor.x, actor.y, actor.zone_id, actor.is_bot,
            actor.dir_x, actor.dir_y, actor.initial_entry});
        co_return std::vector<std::byte> (
          message.bytes ().begin (), message.bytes ().end ());
    }

    fw::task_t<void> restore (
      player_actor_t &actor,
      std::vector<std::byte> payload,
      std::stop_token) override
    {
        const auto restored = zlink::message_t::from (
          std::span<const std::byte> (
            payload.data (), payload.size ()))
                                .parse_json<player_state_t> ();
        actor.x = restored.x;
        actor.y = restored.y;
        actor.zone_id = restored.zone_id;
        actor.is_bot = restored.is_bot;
        actor.dir_x = restored.dir_x;
        actor.dir_y = restored.dir_y;
        actor.initial_entry = restored.initial_entry;
        co_return;
    }
};

} // namespace zlink::samples::zoneworld
