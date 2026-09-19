#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <string>
#include <vector>

namespace fw = zlink::framework;

// --8<-- [start:spot-class]
// A room owns its own state and is addressed by a global SpotId. Messages sent
// to one room run one at a time, so the fields below need no synchronization.
//
// Every C++ user Spot names the actor type it can admit. This room admits none,
// so it names the base type and rejects every join. Actors are a later chapter.
class game_room_t : public fw::spot_t<fw::actor_t>
{
  public:
    explicit game_room_t (fw::spot_context_t context) : _context (std::move (context)) {}

    fw::spot_context_t &context () noexcept override { return _context; }

    const fw::spot_context_t &context () const noexcept override { return _context; }

    // --8<-- [start:spot-handlers]
    // Handlers are member functions named here. A member returning nothing is a
    // send handler; one returning a value answers a request with it.
    void configure () override
    {
        _context.handlers ().add_handler<&game_room_t::post_chat> ();
        _context.handlers ().add_handler<&game_room_t::get_room_state> ();
    }

    void post_chat (const post_chat_t &message)
    {
        auto line = message.player_id + ": " + message.text;
        _chat.push_back (line);
    }

    // The return value is the reply. This handler only reads.
    room_state_t get_room_state (const get_room_state_t &) const
    {
        return room_state_t{_title, _chat};
    }
    // --8<-- [end:spot-handlers]

    // Runs before the room accepts any message. Rejecting here means the create
    // call fails and no room exists. Omit this method to accept every request.
    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &request) override
    {
        const auto &body = request.decode<open_room_t> ();
        _title = body.title;
        co_return fw::spot_create_response_t::accept ();
    }

    // No actor ever joins this room, so the three membership callbacks below say
    // so and do nothing else.
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t ::reject ();
    }

    fw::task_t<void> on_actor_joined (fw::actor_t &) override { co_return; }

    fw::task_t<void> on_leave_actor (fw::actor_t &) override { co_return; }

  private:
    fw::spot_context_t _context;
    std::string _title = "untitled";
    std::vector<std::string> _chat;
};
// --8<-- [end:spot-class]
