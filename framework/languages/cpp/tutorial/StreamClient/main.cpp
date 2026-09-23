#include "../Shared/contracts.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sc = zlink::stream_connector;

namespace
{

long long now_unix_ms ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
             std::chrono::system_clock::now ().time_since_epoch ())
      .count ();
}

template <typename TResult> void require (const TResult &result, const char *what)
{
    if (!result)
        throw std::runtime_error (std::string (what) + ": "
                                  + (result.error () ? result.error ()->message : "failed"));
}

template <typename TMessage> TMessage value_of (sc::result_t<TMessage> result, const char *what)
{
    require (result, what);
    return std::move (result.value ());
}

} // namespace

int main ()
{
    try {
        // --8<-- [start:stream-client]
        // A game client outside the mesh. It references the connector only, never
        // the Framework, and speaks to the port the stream node opened. Every call
        // here is the connector's blocking form.
        sc::connector_options_t options;
        options.endpoint = "tcp://127.0.0.1:7421";
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (5);
        options.wait_timeout = std::chrono::seconds (5);
        options.dispatch_mode = sc::dispatch_mode_t::immediate;

        auto connector = sc::connector_factory_t::create (options);
        connector.codecs ().enable_codec (sc::codec_t::json).use_default_codec (sc::codec_t::json);

        require (connector.connect (), "connect");
        std::cout << "connected: " << std::boolalpha << connector.is_connected () << std::endl;

        // A request waits for its reply. Use send for one-way traffic; the server
        // then answers with write_packet rather than reply_packet.
        const auto sent_at = now_unix_ms ();
        const auto pong = value_of (
          connector.request (ping_t{std::to_string (sent_at)}).submit<pong_t> (), "ping");

        std::cout << "round trip: " << (now_unix_ms () - std::stoll (pong.sent_at_unix_ms)) << "ms"
                  << std::endl;
        // --8<-- [end:stream-client]

        // --8<-- [start:session-actor-client]
        // --8<-- [start:actor-handle-events]
        auto bound_notice = connector.on_actor_bound ([] (const auto &actor) {
            std::cout << "actor bound: " << actor->actor_id () << std::endl;
        });
        auto unbound_notice = connector.on_actor_unbound ([] (const auto &actor) {
            std::cout << "actor unbound: " << actor->actor_id () << std::endl;
        });
        // --8<-- [end:actor-handle-events]
        // With one Actor bound, the connector can send without an Actor handle.
        const auto authenticated = value_of (
          connector.request (authenticate_t{"p1"}).submit<authenticated_t> (), "authenticate");
        std::cout << "bound player: " << authenticated.player_id << std::endl;

        // --8<-- [start:single-actor-send]
        std::promise<sc::message_t<nickname_changed_t>> single_changed;
        auto single_received = single_changed.get_future ();
        {
            auto single_notice = connector.on<nickname_changed_t> (
              [&] (const auto &changed) { single_changed.set_value (changed); });
            connector.send (change_nickname_t{"speedy"}).submit ();
            if (single_received.wait_for (options.wait_timeout) != std::future_status::ready)
                throw std::runtime_error ("NicknameChanged was not received for p1");
            const auto pushed = single_received.get ();
            std::cout << "pushed: " << pushed.payload.nickname
                      << ", actor: " << pushed.actor_id.value_or ("none") << std::endl;
        }
        // --8<-- [end:single-actor-send]

        // A second Actor on the same connection calls for explicit handles.
        const auto authenticated2 = value_of (
          connector.request (authenticate_t{"p2"}).submit<authenticated_t> (), "authenticate");
        std::cout << "bound player: " << authenticated2.player_id << std::endl;

        // --8<-- [start:actor-handle-send]
        auto player = connector.actor (authenticated.player_id);
        auto player2 = connector.actor (authenticated2.player_id);
        if (!player || !player2)
            throw std::runtime_error ("Player Actor was not bound");
        std::cout << "actor handle: " << player->actor_id () << std::endl;
        std::cout << "actor handle: " << player2->actor_id () << std::endl;
        // --8<-- [end:actor-handle-send]

        // --8<-- [start:actor-handle-per-handle-receive]
        std::promise<sc::message_t<nickname_changed_t>> changed1;
        std::promise<sc::message_t<nickname_changed_t>> changed2;
        auto received1 = changed1.get_future ();
        auto received2 = changed2.get_future ();
        auto player_notice = player->on<nickname_changed_t> (
          [&] (const auto &changed) { changed1.set_value (changed); });
        auto player2_notice = player2->on<nickname_changed_t> (
          [&] (const auto &changed) { changed2.set_value (changed); });
        // --8<-- [end:actor-handle-per-handle-receive]

        // --8<-- [start:actor-id-receive]
        // Connector-level callbacks can distinguish the same pushes by ActorId.
        auto actor_id_notice = connector.on<nickname_changed_t> ([] (const auto &message) {
            std::cout << "received actor id: " << message.actor_id.value_or ("none") << std::endl;
        });
        // --8<-- [end:actor-id-receive]

        // Each handle addresses its own player on the shared connection.
        // --8<-- [start:actor-handle-send-call]
        player->send (change_nickname_t{"speedy-p1"}).submit ();
        player2->send (change_nickname_t{"speedy-p2"}).submit ();
        // --8<-- [end:actor-handle-send-call]

        // --8<-- [start:actor-handle-receive]
        if (received1.wait_for (options.wait_timeout) != std::future_status::ready)
            throw std::runtime_error ("NicknameChanged was not received for p1");
        if (received2.wait_for (options.wait_timeout) != std::future_status::ready)
            throw std::runtime_error ("NicknameChanged was not received for p2");
        const auto pushed1 = received1.get ();
        const auto pushed2 = received2.get ();
        std::cout << "pushed: " << pushed1.payload.nickname
                  << ", actor: " << pushed1.actor_id.value_or ("none") << std::endl;
        std::cout << "pushed: " << pushed2.payload.nickname
                  << ", actor: " << pushed2.actor_id.value_or ("none") << std::endl;
        // --8<-- [end:actor-handle-receive]
        // --8<-- [end:session-actor-client]

        require (connector.close (), "close");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "stream client failed: " << error.what () << std::endl;
        return 1;
    }
}
