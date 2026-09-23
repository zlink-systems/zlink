/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using zlink::godot_stream_connector::error_code_t;
using zlink::godot_stream_connector::packet_t;
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

    stream_connector_t alice;
    stream_connector_t bob;
    std::vector<request_result_t> alice_ping;
    std::vector<request_result_t> alice_join;
    std::vector<request_result_t> bob_join;
    std::vector<packet_t> alice_pushes;
    std::vector<packet_t> bob_pushes;
    auto alice_chat =
      alice.on ("ChatNotify", [&] (const packet_t &packet) { alice_pushes.push_back (packet); });
    auto alice_other =
      alice.on ("OtherNotify", [&] (const packet_t &packet) { bob_pushes.push_back (packet); });
    alice.connect (argv[1]);
    bob.connect (argv[1]);

    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0,
                        [&] (const request_result_t &result) { alice_ping.push_back (result); });
    if (!require (until (alice, bob, [&] { return alice_ping.size () == 1; }),
                  "PingReq completion missing")
        || !require (alice_ping[0].reply.has_value () && !alice_ping[0].error_code.has_value (),
                     "PingReq failed")
        || !require (nlohmann::json::parse (payload (*alice_ping[0].reply)).at ("sentAtUnixMs")
                       == "1000",
                     "PingReq response payload wrong")) {
        return 1;
    }

    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0,
                        [&] (const request_result_t &result) { alice_join.push_back (result); });
    bob.request_json ("JoinReq", R"({"name":"bob"})", 5.0,
                      [&] (const request_result_t &result) { bob_join.push_back (result); });
    if (!require (
          until (alice, bob, [&] { return alice_join.size () == 1 && bob_join.size () == 1; }),
          "JoinReq completions missing")
        || !require (alice_ping.size () == 1 && alice_join[0].reply.has_value ()
                       && bob_join[0].reply.has_value (),
                     "JoinReq failed")
        || !require (nlohmann::json::parse (payload (*alice_join[0].reply)).at ("name") == "alice",
                     "JoinReq response payload wrong")) {
        return 1;
    }

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

    alice.connect (argv[1]);
    if (!require (alice_chat.active (), "ChatNotify subscription lost after reconnect")) {
        return 1;
    }
    std::vector<request_result_t> alice_rejoin;
    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0,
                        [&] (const request_result_t &result) { alice_rejoin.push_back (result); });
    if (!require (until (alice, bob, [&] { return alice_rejoin.size () == 1; }),
                  "JoinReq after reconnect missing")
        || !require (alice_rejoin[0].reply.has_value (), "JoinReq after reconnect failed")) {
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
    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0,
                        [&] (const request_result_t &result) { failures.push_back (result); });
    if (!require (failures.empty (), "failed request callback ran before dispatch")
        || !require (until (alice, bob, [&] { return failures.size () == 1; }),
                     "failed request completion missing")
        || !require (!failures[0].reply.has_value ()
                       && failures[0].error_code == error_code_t::disconnected
                       && !failures[0].error_message.empty (),
                     "failed request lost error")) {
        return 1;
    }

    bob.close ();
    std::cout << "godot adapter Ping/Join/Chat/ChatNotify passed\n";
    return 0;
}
