#include "../Shared/contracts.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include <chrono>
#include <cstdlib>
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

namespace receiving_tutorial
{
namespace sc = zlink::stream_connector;
struct leaderboard_update_t
{
    static constexpr const char *packet_name = "LeaderboardUpdate";
    int rank = 0;
};
struct ready_t
{
    static constexpr const char *packet_name = "Ready";
    std::string stage;
};
struct match_found_t
{
    static constexpr const char *packet_name = "MatchFound";
    std::string match_id;
};
struct order_changed_t
{
    static constexpr const char *packet_name = "OrderChanged";
    std::string status;
};
struct receiving_stage_t
{
    static constexpr const char *packet_name = "ReceivingStage";
    std::string stage;
};
inline void from_json (const nlohmann::json &j, leaderboard_update_t &v)
{
    v.rank = j.at ("rank");
}
inline void from_json (const nlohmann::json &j, ready_t &v)
{
    v.stage = j.at ("stage");
}
inline void from_json (const nlohmann::json &j, match_found_t &v)
{
    v.match_id = j.at ("matchId");
}
inline void from_json (const nlohmann::json &j, order_changed_t &v)
{
    v.status = j.at ("status");
}
inline void to_json (nlohmann::json &j, const receiving_stage_t &v)
{
    j = {{"stage", v.stage}};
}

inline void run (const std::string &endpoint)
{
    sc::connector_options_t options;
    options.endpoint = endpoint;
    options.typed_codec = sc::json_typed_codec ();
    options.dispatch_mode = sc::dispatch_mode_t::manual;
    auto connector = sc::connector_factory_t::create (options);
    int handled = 0, frames = 0;
    bool running = true;
    auto render_frame = [&] {
        ++frames;
        running = false;
    };
    auto subscription = connector.on<leaderboard_update_t> ([&] (const auto &) { ++handled; });
    require (connector.connect (), "connect");
    connector.send (receiving_stage_t{"pump"}).submit ();
    value_of (connector.wait_for<ready_t> ().submit (), "ready");
    // --8<-- [start:receiving-pump]
    while (running) {
        connector.dispatch ();
        render_frame ();
    }
    // --8<-- [end:receiving-pump]
    // --8<-- [start:receiving-unsubscribe]
    subscription.unsubscribe ();
    // --8<-- [end:receiving-unsubscribe]
    connector.send (receiving_stage_t{"unsubscribed"}).submit ();
    value_of (connector.wait_for<ready_t> ().submit (), "ready");
    connector.dispatch ();
    connector.send (receiving_stage_t{"match"}).submit ();
    // --8<-- [start:receiving-wait]
    auto found = connector.wait_for<match_found_t> ()
                   .where (
                     [] (const auto &message) { return message.payload.match_id == "match-7f3a"; })
                   .timeout (std::chrono::seconds (30))
                   .submit ();
    // --8<-- [end:receiving-wait]
    // --8<-- [start:receiving-sequence]
    auto quiet = connector.expect_none<order_changed_t> ()
                   .within (std::chrono::milliseconds (100))
                   .submit ();
    require (quiet, "quiet");
    connector.send (receiving_stage_t{"orders"}).submit ();
    auto steps = connector.wait_for_sequence<order_changed_t> ()
                   .expect ([] (const auto &m) { return m.payload.status == "paid"; })
                   .expect ([] (const auto &m) { return m.payload.status == "shipped"; })
                   .timeout (std::chrono::seconds (2))
                   .submit ();
    // --8<-- [end:receiving-sequence]
    // --8<-- [start:receiving-count]
    auto count = connector.received_count ("LeaderboardUpdate");
    // --8<-- [end:receiving-count]
    auto match = value_of (std::move (found), "match");
    auto orders = value_of (std::move (steps), "orders");
    if (handled != 1 || frames != 1 || count != 2 || match.payload.match_id != "match-7f3a"
        || orders.size () != 2)
        throw std::runtime_error ("Receiving tutorial result did not match expected messages.");
    std::cout << "receiving: handler=" << handled << ", frames=" << frames
              << ", match=" << match.payload.match_id << ", sequence=" << orders[0].payload.status
              << "," << orders[1].payload.status << ", count=" << count << std::endl;
    require (connector.close (), "close");
}
}


int main (int argc, char **argv)
{
    try {
        if (argc > 1 && std::string (argv[1]) == "--receiving") {
            const char *endpoint = std::getenv ("STREAM_RECEIVING_ENDPOINT");
            if (!endpoint)
                throw std::runtime_error ("STREAM_RECEIVING_ENDPOINT is required.");
            receiving_tutorial::run (endpoint);
            return 0;
        }
        // --8<-- [start:stream-client]
        // A game client outside the mesh. It references the connector only, never
        // the Framework, and speaks to the port the stream node opened. Every call
        // here is the connector's blocking form.
        sc::connector_options_t options;
        options.typed_codec = sc::json_typed_codec ();
        options.endpoint = "tcp://127.0.0.1:7421";
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (5);
        options.wait_timeout = std::chrono::seconds (5);
        options.dispatch_mode = sc::dispatch_mode_t::immediate;

        auto connector = sc::connector_factory_t::create (options);

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
            // --8<-- [start:typed-receive]
            auto single_notice = connector.on<nickname_changed_t> (
              [&] (const auto &changed) { single_changed.set_value (changed); });
            // --8<-- [end:typed-receive]
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
