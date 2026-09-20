#pragma once

#include "../actors/player.hpp"
#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <string_view>

namespace fw = zlink::framework;

// --8<-- [start:entry-spot]
// Every new player lands here before joining a room, and returns here after
// leaving one. A node that hosts players registers exactly one of these.
class lobby_spot_t : public fw::entry_spot_t<player_t>
{
  public:
    explicit lobby_spot_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    // --8<-- [start:actor-handlers]
    // A message addressed to a player runs inside the Spot the player currently
    // occupies, so a handler receives both the Spot and the player.
    void configure () override
    {
        _context.handlers ().add_actor_send<&lobby_spot_t::change_nickname> ();
        _context.handlers ().add_actor_request<&lobby_spot_t::get_player> ();
    }

    // --8<-- [start:actor-send-handler]
    fw::task_t<void>
    change_nickname (player_t &player, fw::message_context_t &, const change_nickname_t &message)
    {
        player.rename (message.nickname);

        // --8<-- [start:actor-push]
        // Pushes over the connection bound to this player. The same handler also runs
        // on an HTTP path with no bound connection, where push ends with InvalidOperation.
        // Rename is already complete, so only that failure is discarded.
        try {
            co_await player.context ()
              .bound_session ()
              .send (nickname_changed_t{player.nickname})
              .async ();
        }
        catch (const fw::framework_exception_t &error) {
            if (error.kind () != fw::framework_error_kind_t::invalid_operation)
                throw;
        }
        // --8<-- [end:actor-push]
    }
    // --8<-- [end:actor-send-handler]

    // --8<-- [start:actor-request-handler]
    player_info_t get_player (const player_t &player, fw::message_context_t &, const get_player_t &)
    {
        return player_info_t{std::string (player.context ().actor_id ().value ()), player.nickname};
    }
    // --8<-- [end:actor-request-handler]
    // --8<-- [end:actor-handlers]

    // Runs when a brand-new player is created. Rejecting here means the create
    // call fails and no player exists.
    fw::task_t<fw::actor_create_response_t> on_create_actor (player_t &,
                                                             const fw::message_t &) override
    {
        co_return fw::actor_create_response_t::accept ();
    }

    // The lobby is where every player belongs by default, so it admits each one.
    // A room would inspect the request here and reject the ones it will not take.
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }

    fw::task_t<void> on_actor_joined (player_t &) override { co_return; }

    fw::task_t<void> on_leave_actor (player_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};
// --8<-- [end:entry-spot]
