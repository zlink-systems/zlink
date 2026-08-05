/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../../../Shared/Contracts/messages.hpp"

#include <zlink/framework.hpp>

#include <memory>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

struct player_actor_t : framework::actor_t
{
    mutable std::string actor_id;
    mutable std::unique_ptr<actor_context_t> actor_context;
    std::string display_name;
    mutable bool destroy_after_entry_spot_join = false;
    mutable bool disconnected = false;

    player_actor_t () = default;
    player_actor_t (const player_actor_t &other) :
        actor_id (other.actor_id), display_name (other.display_name),
        destroy_after_entry_spot_join (other.destroy_after_entry_spot_join),
        disconnected (other.disconnected)
    {
    }
    player_actor_t &operator= (const player_actor_t &other)
    {
        if (this != &other) {
            actor_id = other.actor_id;
            display_name = other.display_name;
            destroy_after_entry_spot_join = other.destroy_after_entry_spot_join;
            disconnected = other.disconnected;
            actor_context.reset ();
        }
        return *this;
    }
    player_actor_t (player_actor_t &&) noexcept = default;
    player_actor_t &operator= (player_actor_t &&) noexcept = default;

    void set_actor_ref (const actor_ref_t &actor_ref) const
    {
        actor_id = std::string (actor_ref.actor_id ().value ());
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

    template <typename TNotify> void push (const TNotify &notify) const
    {
        actor_context->bound_session ().send (notify).submit ();
    }
};

} // namespace zlink::samples::bingo
