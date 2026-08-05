/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <zlink/framework.hpp>

#include "../../../../Configuration/sample_names.hpp"
#include "../../../../../Shared/Contracts/messages.hpp"

namespace zlink::samples::tictactoe
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

struct player_actor_t : framework::actor_t
{
    std::string actor_id;
    mutable std::string node_rid;
    mutable unsigned long long generation = 1;
    mutable bool destroy_after_entry_spot_join = false;
    mutable bool disconnected = false;
    mutable player_info_t player;
    mutable std::deque<std::string> pending_join_rooms;
    mutable std::set<std::pair<std::uint64_t, std::uint64_t>>
      processed_join_operations;
    mutable std::unique_ptr<actor_context_t> actor_context;

    player_actor_t () = default;
    explicit player_actor_t (std::string value) : actor_id (std::move (value)) {}
    player_actor_t (const player_actor_t &other) :
        actor_id (other.actor_id), node_rid (other.node_rid), generation (other.generation),
        destroy_after_entry_spot_join (other.destroy_after_entry_spot_join),
        disconnected (other.disconnected), player (other.player),
        pending_join_rooms (other.pending_join_rooms),
        processed_join_operations (other.processed_join_operations)
    {
    }
    player_actor_t &operator= (const player_actor_t &other)
    {
        if (this != &other) {
            actor_id = other.actor_id;
            node_rid = other.node_rid;
            generation = other.generation;
            destroy_after_entry_spot_join = other.destroy_after_entry_spot_join;
            disconnected = other.disconnected;
            player = other.player;
            pending_join_rooms = other.pending_join_rooms;
            processed_join_operations = other.processed_join_operations;
            actor_context.reset ();
        }
        return *this;
    }
    player_actor_t (player_actor_t &&) noexcept = default;
    player_actor_t &operator= (player_actor_t &&) noexcept = default;

    void set_actor_ref (const actor_ref_t &actor_ref) const
    {
        node_rid = std::string (actor_ref.node_rid ().value ());
        generation = actor_ref.object_generation ();
    }

    void set_actor_context (actor_context_t actor_context) const
    {
        this->actor_context =
          std::make_unique<actor_context_t> (std::move (actor_context));
    }

    actor_context_t &context () noexcept override { return *actor_context; }
    const actor_context_t &context () const noexcept override { return *actor_context; }

    void mark_for_destroy_after_room_leave () const { destroy_after_entry_spot_join = true; }

    void mark_disconnected () const { disconnected = true; }

    void apply_player (player_info_t value) const { player = std::move (value); }

    void track_deferred_join (std::string room_id) const
    {
        pending_join_rooms.push_back (std::move (room_id));
    }

    task_t<void>
    on_join_completed (const actor_join_completion_t &completion) override
    {
        const auto operation = std::visit (
          [] (const auto &value) {
              return std::pair{value.operation_id_high,
                               value.operation_id_low};
          },
          completion);
        if (processed_join_operations.contains (operation))
            co_return;

        const auto room_id = pending_join_rooms.empty () ? std::string{} : pending_join_rooms.front ();
        if (const auto *accepted =
              std::get_if<actor_join_accepted_t> (&completion);
            accepted != nullptr && accepted->reply) {
            const auto joined =
              accepted->reply->decode<tictactoe_game_join_res_t> ();
            actor_context->bound_session ()
              .send (join_game_res_t{joined.state})
              .submit ();
        } else if (std::holds_alternative<actor_join_rejected_t> (completion)) {
            actor_context->bound_session ()
              .send (join_game_failed_notify_t{room_id, "room admission rejected"})
              .submit ();
        } else if (std::holds_alternative<actor_join_failed_t> (completion)) {
            actor_context->bound_session ()
              .send (join_game_failed_notify_t{room_id, "room admission failed"})
              .submit ();
        }
        processed_join_operations.emplace (operation);
        if (!pending_join_rooms.empty ())
            pending_join_rooms.pop_front ();
        co_return;
    }

    template <typename TNotify> void push (const TNotify &notify) const
    {
        actor_context->bound_session ().send (notify).submit ();
    }

    player_info_t require_player () const
    {
        if (player.actor_id.empty ()) {
            return {actor_id, actor_id, sample_names_t::required_level, 0};
        }
        return player;
    }

    int increment_wins () const { return ++player.wins; }
};

inline void to_json (nlohmann::json &json, const player_actor_t &value)
{
    json = nlohmann::json{{"actorId", value.actor_id},
                          {"nodeRid", value.node_rid},
                          {"generation", value.generation},
                          {"destroyAfterEntrySpotJoin", value.destroy_after_entry_spot_join},
                          {"disconnected", value.disconnected},
                          {"player", value.player}};
}

inline void from_json (const nlohmann::json &json, player_actor_t &value)
{
    value.actor_id = json.value ("actorId", std::string{});
    value.node_rid = json.value ("nodeRid", std::string{});
    value.generation = json.value ("generation", 1ull);
    value.destroy_after_entry_spot_join = json.value ("destroyAfterEntrySpotJoin", false);
    value.disconnected = json.value ("disconnected", false);
    value.player = json.value ("player", player_info_t{});
}

struct player_actor_factory_t final
    : framework::actor_factory_t<player_actor_t>
{
    player_actor_t create (std::string actor_id) const
    {
        return player_actor_t (std::move (actor_id));
    }

    framework::task_t<std::shared_ptr<player_actor_t>>
    create (actor_context_t context,
            std::stop_token) override
    {
        auto actor = std::make_shared<player_actor_t> (
          std::string (context.actor_ref ().actor_id ().value ()));
        actor->set_actor_ref (context.actor_ref ());
        actor->set_actor_context (std::move (context));
        co_return actor;
    }
};

struct move_packet_t
{
    int cell = 0;
};

} // namespace zlink::samples::tictactoe
