#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace fw = zlink::framework;

// --8<-- [start:session-class]
// One connected game client. Callbacks for the same connection run in order.
// C++ has no session handler registry: every packet arrives at on_packet and
// this class decides what to do with it by name.
class game_session_t final : public fw::packet_stream_session_t
{
  public:
    fw::task_t<void> on_connected (fw::stream_t &stream) override
    {
        std::cout << "client connected: " << stream.session_id () << std::endl;
        co_return;
    }

    fw::task_t<void> on_disconnected (fw::stream_t &stream) override
    {
        std::cout << "client disconnected: " << stream.session_id () << std::endl;
        _player_id.reset ();
        co_return;
    }

    fw::task_t<void> on_error (fw::stream_t &stream, const fw::stream_error_t &) override
    {
        std::cout << "stream error on " << stream.session_id () << std::endl;
        co_return;
    }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);

        // --8<-- [start:session-handler]
        if (packet == ping_t::packet_name) {
            const auto request = payload.parse_json<ping_t> ();

            // reply_packet answers a request. To push to a client that is not
            // waiting for one, use write_packet instead.
            stream.reply_packet (zlink::message_t::from_json (pong_t{request.sent_at_unix_ms}))
              .async ();
            co_return;
        }
        // --8<-- [end:session-handler]

        // --8<-- [start:session-actor-bind]
        // Ties this connection to one player. After this, packets handled by no
        // branch above reach that player, and the player can push to this
        // connection.
        if (packet == authenticate_t::packet_name) {
            const auto request = payload.parse_json<authenticate_t> ();

            // A returning client finds its existing player rather than a new one.
            auto &actors = stream.actors ();
            auto located = actors.get_or_create (
              "player", request.player_id, create_player_t{request.player_id});
            if (!located)
                throw fw::framework_exception_t (located.error_kind (),
                                                 "Player creation was rejected.");

            auto bound = co_await actors.bind_or_get (located.value ().ref ()).async ();
            _player_id = std::string (bound.actor_id ());

            stream.reply_packet (zlink::message_t::from_json (authenticated_t{*_player_id}))
              .async ();
            co_return;
        }
        // --8<-- [end:session-actor-bind]

        // --8<-- [start:session-actor-relay]
        // Anything this class does not answer itself is forwarded to the player
        // bound to this connection, which is why authentication has to come first.
        if (!_player_id)
            throw fw::framework_exception_t (fw::framework_error_kind_t::invalid_operation,
                                             "Authenticate before sending player packets.");

        auto player = stream.actors ().find (*_player_id);
        if (!player)
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "The bound player is gone.");

        co_await player->relay (packet, payload);
        // --8<-- [end:session-actor-relay]
    }

  private:
    std::optional<std::string> _player_id;
};
// --8<-- [end:session-class]
