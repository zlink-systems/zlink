/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <deque>
#include <format>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace zlink::samples::zoneworld
{

namespace fw = zlink::framework;

inline std::string framework_error_name (fw::framework_error_kind_t kind)
{
    switch (kind) {
        case fw::framework_error_kind_t::not_found:
            return errors_t::not_found;
        case fw::framework_error_kind_t::already_exists:
            return errors_t::already_exists;
        case fw::framework_error_kind_t::type_mismatch:
            return errors_t::type_mismatch;
        case fw::framework_error_kind_t::not_configured:
            return errors_t::not_configured;
        case fw::framework_error_kind_t::rejected:
            return errors_t::rejected;
        case fw::framework_error_kind_t::unavailable:
            return errors_t::unavailable;
        case fw::framework_error_kind_t::deadline_exceeded:
            return errors_t::deadline_exceeded;
        case fw::framework_error_kind_t::shutting_down:
            return errors_t::shutting_down;
        case fw::framework_error_kind_t::protocol_error:
            return errors_t::protocol_error;
        case fw::framework_error_kind_t::invalid_operation:
            return errors_t::invalid_operation;
        case fw::framework_error_kind_t::data_lost:
            return errors_t::data_lost;
        case fw::framework_error_kind_t::internal_failure:
            return errors_t::internal_failure;
    }
    return errors_t::internal_failure;
}

class player_actor_t final : public fw::actor_t
{
  public:
    explicit player_actor_t (fw::actor_context_t context) : _context (std::move (context))
    {
        player_id = std::string (_context.actor_ref ().actor_id ().value ());
    }

    fw::actor_context_t &context () noexcept override { return _context; }

    const fw::actor_context_t &context () const noexcept override { return _context; }

    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        // --8<-- [start:doc-zw-join-completed]
        const auto operation = std::visit (
          [] (const auto &value) {
              return std::pair{value.operation_id_high, value.operation_id_low};
          },
          completion);
        if (completed_join_operations.contains (operation))
            co_return;
        if (std::holds_alternative<fw::actor_join_accepted_t> (completion)) {
            const std::string line = std::format (
              "zoneworld-join-accepted player={} zone={} pending-initial={} bot={}\n",
              player_id,
              zone_id,
              pending_initial_entry ? "true" : "false",
              is_bot ? "true" : "false");
            std::cerr << line;
            if (pending_initial_entry && !is_bot) {
                co_await _context.bound_session ()
                  .send (join_world_res_t{player_id, zone_id, x, y, std::nullopt})
                  .async ();
                std::cout << "zoneworld-join-response player=" << player_id << " zone=" << zone_id
                          << std::endl;
            }
            pending_join = false;
            initial_entry = false;
            pending_crash_probe = false;
        } else if (const auto *failed = std::get_if<fw::actor_join_failed_t> (&completion)) {
            const std::string line = std::format ("zoneworld-join-failed player={} kind={}\n",
                                                  player_id,
                                                  static_cast<int> (failed->error_kind));
            std::cerr << line;
            const auto error = framework_error_name (failed->error_kind);
            if (pending_crash_probe && !is_bot) {
                co_await _context.bound_session ()
                  .send (crash_relocation_probe_res_t{error})
                  .async ();
            } else if (pending_initial_entry && !is_bot) {
                co_await _context.bound_session ()
                  .send (join_world_res_t{player_id, zone_id, x, y, error})
                  .async ();
            }
            pending_join = false;
            pending_crash_probe = false;
        } else if (const auto *rejected = std::get_if<fw::actor_join_rejected_t> (&completion)) {
            const std::string line = std::format ("zoneworld-join-rejected player={}\n", player_id);
            std::cerr << line;
            auto reason = std::string (reject_reason_t::zone_maintenance);
            if (rejected->reply) {
                reason = rejected->reply->decode<enter_zone_res_t> ().error.value_or (reason);
            }
            if (is_bot) {
                dir_x = -dir_x;
                dir_y = -dir_y;
            } else if (pending_initial_entry) {
                co_await _context.bound_session ()
                  .send (join_world_res_t{player_id, zone_id, x, y, reason})
                  .async ();
            } else {
                co_await _context.bound_session ()
                  .send (move_rejected_notify_t{reason, x, y})
                  .async ();
            }
            pending_join = false;
            pending_crash_probe = false;
        }
        remember_join_operation (operation);
        // --8<-- [end:doc-zw-join-completed]
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
    bool pending_join = false;
    bool pending_initial_entry = false;
    bool pending_crash_probe = false;
    int pending_x = 25;
    int pending_y = 25;
    std::string pending_zone_id = "zone-nw";
    std::set<std::pair<std::uint64_t, std::uint64_t>> completed_join_operations;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> completed_join_operation_order;

  private:
    static constexpr std::size_t completed_join_operation_retention = 256;

    void remember_join_operation (const std::pair<std::uint64_t, std::uint64_t> &operation)
    {
        if (!completed_join_operations.emplace (operation).second)
            return;
        completed_join_operation_order.push_back (operation);
        while (completed_join_operation_order.size () > completed_join_operation_retention) {
            completed_join_operations.erase (completed_join_operation_order.front ());
            completed_join_operation_order.pop_front ();
        }
    }

    fw::actor_context_t _context;
};

class player_actor_factory_t final : public fw::actor_factory_t<player_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<player_actor_t>> create (fw::actor_context_t context,
                                                        std::stop_token) override
    {
        co_return std::make_shared<player_actor_t> (std::move (context));
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
    bool pending_join = false;
    bool pending_initial_entry = false;
    bool pending_crash_probe = false;
    int pending_x = 25;
    int pending_y = 25;
    std::string pending_zone_id = "zone-nw";
    std::deque<std::pair<std::uint64_t, std::uint64_t>> completed_join_operation_order;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (player_state_t,
                                    x,
                                    y,
                                    zone_id,
                                    is_bot,
                                    dir_x,
                                    dir_y,
                                    initial_entry,
                                    pending_join,
                                    pending_initial_entry,
                                    pending_crash_probe,
                                    pending_x,
                                    pending_y,
                                    pending_zone_id,
                                    completed_join_operation_order)

class player_relocation_adapter_t final : public fw::actor_relocation_adapter_t<player_actor_t>
{
  public:
    // --8<-- [start:doc-zw-actor-capture]
    fw::task_t<std::vector<std::byte>> capture (player_actor_t &actor, std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (
          player_state_t{actor.x,
                         actor.y,
                         actor.zone_id,
                         actor.is_bot,
                         actor.dir_x,
                         actor.dir_y,
                         actor.initial_entry,
                         actor.pending_join,
                         actor.pending_initial_entry,
                         actor.pending_crash_probe,
                         actor.pending_x,
                         actor.pending_y,
                         actor.pending_zone_id,
                         actor.completed_join_operation_order});
        co_return std::vector<std::byte> (message.bytes ().begin (), message.bytes ().end ());
    }
    // --8<-- [end:doc-zw-actor-capture]

    fw::task_t<void>
    restore (player_actor_t &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        const auto restored = zlink::message_t::from (
                                std::span<const std::byte> (payload.data (), payload.size ()))
                                .parse_json<player_state_t> ();
        actor.x = restored.x;
        actor.y = restored.y;
        actor.zone_id = restored.zone_id;
        actor.is_bot = restored.is_bot;
        actor.dir_x = restored.dir_x;
        actor.dir_y = restored.dir_y;
        actor.initial_entry = restored.initial_entry;
        actor.pending_join = restored.pending_join;
        actor.pending_initial_entry = restored.pending_initial_entry;
        actor.pending_crash_probe = restored.pending_crash_probe;
        actor.pending_x = restored.pending_x;
        actor.pending_y = restored.pending_y;
        actor.pending_zone_id = restored.pending_zone_id;
        actor.completed_join_operation_order = std::move (restored.completed_join_operation_order);
        actor.completed_join_operations = {actor.completed_join_operation_order.begin (),
                                           actor.completed_join_operation_order.end ()};
        co_return;
    }
};

} // namespace zlink::samples::zoneworld
