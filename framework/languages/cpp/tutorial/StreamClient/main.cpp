#include "../Shared/contracts.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include <chrono>
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
        options.dispatch_mode = sc::dispatch_mode_t::manual;

        auto connector = sc::connector_factory_t::create (options);
        connector.codecs ()
          .enable_codec (sc::codec_t::json)
          .use_default_codec (sc::codec_t::json);

        require (connector.connect (), "connect");
        std::cout << "connected: " << std::boolalpha << connector.is_connected () << std::endl;

        // A request waits for its reply. Use send for one-way traffic; the server
        // then answers with write_packet rather than reply_packet.
        const auto sent_at = now_unix_ms ();
        const auto pong = value_of (
          connector.request (ping_t{std::to_string (sent_at)}).submit<pong_t> (), "ping");

        std::cout << "round trip: " << (now_unix_ms () - std::stoll (pong.sent_at_unix_ms))
                  << "ms" << std::endl;
        // --8<-- [end:stream-client]

        // --8<-- [start:session-actor-client]
        // Binds this connection to a player. Until then the server has no player
        // to forward packets to.
        const auto authenticated = value_of (
          connector.request (authenticate_t{"p1"}).submit<authenticated_t> (), "authenticate");

        std::cout << "bound player: " << authenticated.player_id << std::endl;

        // No branch of the session answers this packet, so the session relays it
        // to the bound player, whose handler pushes the result back over this same
        // connection. This connector uses manual dispatch, so a push that lands
        // before the wait starts is queued rather than dropped, and the wait can
        // follow the send.
        connector.send (change_nickname_t{"speedy"}).submit ();

        const auto changed =
          value_of (connector.wait_for<nickname_changed_t> ().submit (), "nickname push");

        std::cout << "pushed: " << changed.payload.nickname << std::endl;
        // --8<-- [end:session-actor-client]

        require (connector.close (), "close");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "stream client failed: " << error.what () << std::endl;
        return 1;
    }
}
