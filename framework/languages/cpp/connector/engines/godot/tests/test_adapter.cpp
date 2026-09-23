/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using zlink::godot_stream_connector::error_code_t;
using zlink::godot_stream_connector::packet_t;
using zlink::godot_stream_connector::reply_received_context_t;
using zlink::godot_stream_connector::request_result_t;
using zlink::godot_stream_connector::stream_connector_t;
using zlink::godot_stream_connector::subscription_t;

namespace
{

std::string payload (const packet_t &packet)
{
    return {packet.payload.begin (), packet.payload.end ()};
}

template <typename Predicate>
bool until (stream_connector_t &alice, stream_connector_t &bob, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        alice.dispatch ();
        bob.dispatch ();
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return false;
}

bool require (bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: test_adapter <engine-lobby-ws-endpoint>\n";
        return 2;
    }

    zlink::context_t raw_context;
    zlink::stream_socket_t raw_server (raw_context);
    raw_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    raw_server.options ().notify (false);
    raw_server.bind ("tcp://127.0.0.1:0");
    stream_connector_t raw_client;
    std::vector<reply_received_context_t> replaced_replies;
    std::vector<request_result_t> replaced_completions;
    auto raw_reply_hook = raw_client.on_reply_received (
      [&] (const reply_received_context_t &context) { replaced_replies.push_back (context); });
    raw_client.connect (raw_server.options ().last_endpoint ());
    raw_client.request_json (
      "GodotPendingReq", R"({"probe":true})", 5.0,
      [&] (const request_result_t &result) { replaced_completions.push_back (result); });
    zlink::received_t inbound;
    if (!require (raw_server.recv (inbound) == 0, "raw server missed pending Godot request")) {
        return 1;
    }
    inbound.close ();
    raw_client.connect (raw_server.options ().last_endpoint ());
    if (!require (replaced_replies.empty () && replaced_completions.empty (),
                  "replacement callbacks ran before adapter dispatch")
        || !require (until (raw_client, raw_client,
                            [&] {
                                return replaced_replies.size () == 1
                                       && replaced_completions.size () == 1;
                            }),
                     "replacement callbacks missing")
        || !require (!replaced_replies[0].succeeded && replaced_replies[0].error
                       && replaced_replies[0].error->code == error_code_t::disconnected
                       && replaced_completions[0].error_code == error_code_t::disconnected,
                     "pending Godot request lost reconnect failure")) {
        return 1;
    }
    raw_client.close ();

    stream_connector_t alice;
    stream_connector_t bob;
    std::vector<request_result_t> alice_ping;
    std::vector<request_result_t> alice_join;
    std::vector<request_result_t> bob_join;
    std::vector<packet_t> alice_pushes;
    std::vector<packet_t> bob_pushes;
    std::vector<std::string> sending_names;
    std::vector<reply_received_context_t> reply_hooks;
    auto sending_hook = alice.on_request_sending (
      [&] (zlink::godot_stream_connector::request_sending_context_t &context) {
          sending_names.push_back (context.request_packet_name);
          context.set_metadata ("godotHook", "from-client");
      });
    auto reply_hook = alice.on_reply_received (
      [&] (const reply_received_context_t &context) { reply_hooks.push_back (context); });
    auto throwing_sending_hook =
      alice.on_request_sending ([] (zlink::godot_stream_connector::request_sending_context_t &) {
          throw std::runtime_error ("expected Godot sending hook failure");
      });
    auto throwing_reply_hook = alice.on_reply_received ([] (const reply_received_context_t &) {
        throw std::runtime_error ("expected Godot reply hook failure");
    });
    auto alice_chat =
      alice.on ("ChatNotify", [&] (const packet_t &packet) { alice_pushes.push_back (packet); });
    auto alice_other =
      alice.on ("OtherNotify", [&] (const packet_t &packet) { bob_pushes.push_back (packet); });
    alice.connect (argv[1]);
    bob.connect (argv[1]);

    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0,
                        [&] (const request_result_t &result) { alice_ping.push_back (result); });
    if (!require (sending_names.size () == 1 && sending_names[0] == "PingReq",
                  "sending hook was not synchronous")
        || !require (reply_hooks.empty (), "reply hook ran before dispatch")
        || !require (
          until (alice, bob, [&] { return alice_ping.size () == 1 && reply_hooks.size () == 1; }),
          "PingReq completion missing")
        || !require (reply_hooks[0].request_packet_name == "PingReq" && reply_hooks[0].succeeded
                       && reply_hooks[0].reply && !reply_hooks[0].error,
                     "PingReq reply hook context wrong")
        || !require (alice_ping[0].reply.has_value () && !alice_ping[0].error_code.has_value (),
                     "PingReq failed")
        || !require (nlohmann::json::parse (payload (*alice_ping[0].reply)).at ("sentAtUnixMs")
                       == "1000",
                     "PingReq response payload wrong")) {
        return 1;
    }
    throwing_sending_hook.unsubscribe ();
    throwing_reply_hook.unsubscribe ();

    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0,
                        [&] (const request_result_t &result) { alice_join.push_back (result); });
    bob.request_json ("JoinReq", R"({"name":"bob"})", 5.0,
                      [&] (const request_result_t &result) { bob_join.push_back (result); });
    if (!require (sending_names.size () == 2 && sending_names[1] == "JoinReq",
                  "sending hook missed JoinReq")) {
        return 1;
    }
    if (!require (until (alice, bob,
                         [&] {
                             return alice_join.size () == 1 && bob_join.size () == 1
                                    && reply_hooks.size () == 2;
                         }),
                  "JoinReq completions missing")
        || !require (alice_ping.size () == 1 && alice_join[0].reply.has_value ()
                       && bob_join[0].reply.has_value (),
                     "JoinReq failed")
        || !require (nlohmann::json::parse (payload (*alice_join[0].reply)).at ("name") == "alice",
                     "JoinReq response payload wrong")) {
        return 1;
    }
    sending_hook.unsubscribe ();
    reply_hook.unsubscribe ();

    alice.send_json ("ChatMsg", R"({"text":"hello"})");
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    if (!require (alice_pushes.empty () && bob_pushes.empty (),
                  "push callback ran before dispatch")) {
        return 1;
    }
    if (!require (until (alice, bob, [&] { return alice_pushes.size () == 1; }),
                  "subscribed ChatNotify push missing")
        || !require (alice_pushes[0].name == "ChatNotify", "ChatNotify push name wrong")
        || !require (nlohmann::json::parse (payload (alice_pushes[0])).at ("text") == "hello",
                     "ChatNotify push payload wrong")) {
        return 1;
    }

    const auto quiet_until = std::chrono::steady_clock::now () + std::chrono::milliseconds (500);
    while (std::chrono::steady_clock::now () < quiet_until) {
        alice.dispatch ();
        bob.dispatch ();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (!require (bob_pushes.empty (), "different packet name reached callback")) {
        return 1;
    }

    std::vector<reply_received_context_t> rejoin_hooks;
    auto rejoin_hook = alice.on_reply_received (
      [&] (const reply_received_context_t &context) { rejoin_hooks.push_back (context); });
    alice.connect (argv[1]);
    if (!require (alice_chat.active (), "ChatNotify subscription lost after reconnect")) {
        return 1;
    }
    std::vector<request_result_t> alice_rejoin;
    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0,
                        [&] (const request_result_t &result) { alice_rejoin.push_back (result); });
    if (!require (until (alice, bob,
                         [&] { return alice_rejoin.size () == 1 && rejoin_hooks.size () == 1; }),
                  "JoinReq after reconnect missing")
        || !require (alice_rejoin[0].reply.has_value (), "JoinReq after reconnect failed")
        || !require (rejoin_hooks[0].request_packet_name == "JoinReq" && rejoin_hooks[0].succeeded,
                     "reply hook did not survive reconnect")) {
        return 1;
    }
    alice.send_json ("ChatMsg", R"({"text":"reconnected"})");
    if (!require (until (alice, bob, [&] { return alice_pushes.size () == 2; }),
                  "ChatNotify registration did not survive reconnect")) {
        return 1;
    }

    subscription_t self_subscription;
    std::size_t self_calls = 0;
    self_subscription = alice.on ("ChatNotify", [&] (const packet_t &) {
        ++self_calls;
        self_subscription.unsubscribe ();
    });
    alice.send_json ("ChatMsg", R"({"text":"self-unsubscribe"})");
    if (!require (until (alice, bob, [&] { return self_calls == 1; }),
                  "self-unsubscribe callback missing")
        || !require (!self_subscription.active (), "self-unsubscribe handle remained active")) {
        return 1;
    }
    alice.send_json ("ChatMsg", R"({"text":"after-unsubscribe"})");
    if (!require (until (alice, bob, [&] { return alice_pushes.size () == 4; }),
                  "follow-up ChatNotify missing")
        || !require (self_calls == 1, "self-unsubscribed callback ran again")) {
        return 1;
    }

    alice.close ();
    std::vector<request_result_t> failures;
    std::vector<reply_received_context_t> failure_hooks;
    auto failure_hook = alice.on_reply_received (
      [&] (const reply_received_context_t &context) { failure_hooks.push_back (context); });
    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0,
                        [&] (const request_result_t &result) { failures.push_back (result); });
    if (!require (failures.empty (), "failed request callback ran before dispatch")
        || !require (until (alice, bob, [&] { return failures.size () == 1; }),
                     "failed request completion missing")
        || !require (!failures[0].reply.has_value ()
                       && failures[0].error_code == error_code_t::disconnected
                       && !failures[0].error_message.empty (),
                     "failed request lost error")
        || !require (failure_hooks.size () == 1 && !failure_hooks[0].succeeded
                       && failure_hooks[0].error
                       && failure_hooks[0].error->code == error_code_t::disconnected,
                     "failed request reply hook lost error")) {
        return 1;
    }

    bob.close ();
    std::cout << "godot adapter Ping/Join/Chat/ChatNotify passed\n";
    return 0;
}
