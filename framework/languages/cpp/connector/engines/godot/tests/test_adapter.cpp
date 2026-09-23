/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using zlink::godot_stream_connector::packet_t;
using zlink::godot_stream_connector::stream_connector_t;

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
    std::vector<packet_t> alice_requests;
    std::vector<packet_t> bob_requests;
    std::vector<packet_t> alice_pushes;
    std::vector<packet_t> bob_pushes;
    alice.on_request_completed (
      [&] (const packet_t &packet) { alice_requests.push_back (packet); });
    bob.on_request_completed ([&] (const packet_t &packet) { bob_requests.push_back (packet); });
    alice.on_packet ([&] (const packet_t &packet) { alice_pushes.push_back (packet); });
    bob.on_packet ([&] (const packet_t &packet) { bob_pushes.push_back (packet); });

    alice.subscribe ("ChatNotify");
    alice.connect (argv[1]);
    bob.connect (argv[1]);

    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0);
    if (!require (until (alice, bob, [&] { return alice_requests.size () == 1; }),
                  "PingReq completion missing")
        || !require (alice_requests[0].name == "PingReq", "PingReq completion name wrong")
        || !require (nlohmann::json::parse (payload (alice_requests[0])).at ("sentAtUnixMs")
                       == "1000",
                     "PingReq response payload wrong")) {
        return 1;
    }

    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0);
    bob.request_json ("JoinReq", R"({"name":"bob"})", 5.0);
    if (!require (until (alice, bob,
                         [&] { return alice_requests.size () == 2 && bob_requests.size () == 1; }),
                  "JoinReq completions missing")
        || !require (alice_requests[1].name == "JoinReq" && bob_requests[0].name == "JoinReq",
                     "JoinReq completion name wrong")
        || !require (nlohmann::json::parse (payload (alice_requests[1])).at ("name") == "alice",
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
    if (!require (bob_pushes.empty (), "unsubscribed ChatNotify reached callback")) {
        return 1;
    }

    alice.close ();
    bob.close ();
    std::cout << "godot adapter Ping/Join/Chat/ChatNotify passed\n";
    return 0;
}
