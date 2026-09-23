/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using zlink::godot_stream_connector::packet_t;
using zlink::godot_stream_connector::reply_received_context_t;
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

    zlink::context_t context;
    zlink::stream_socket_t raw_server (context);
    raw_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    raw_server.options ().notify (false);
    raw_server.bind ("tcp://127.0.0.1:0");
    stream_connector_t raw_client;
    int raw_sending_calls = 0;
    std::vector<reply_received_context_t> replacement_replies;
    auto replacement_hook = raw_client.on_reply_received (
      [&] (const reply_received_context_t &hook) { replacement_replies.push_back (hook); });
    auto raw_sending_hook = raw_client.on_request_sending (
      [&] (zlink::godot_stream_connector::request_sending_context_t &hook) {
          ++raw_sending_calls;
          if (hook.request_packet_name == "GodotHookReq" && !hook.actor_id) {
              hook.set_metadata ("godot-hook", "present");
          }
      });
    raw_client.connect (raw_server.options ().last_endpoint ());
    raw_client.request_json ("GodotHookReq", R"({"probe":true})", 5.0);
    if (!require (raw_sending_calls == 1, "sending hook did not run in request call")) {
        return 1;
    }
    zlink::received_t inbound;
    if (!require (raw_server.recv (inbound) == 0, "raw server missed Godot hook request")) {
        return 1;
    }
    const auto frame = inbound.parts ()[0].to_string ();
    const auto header_size = frame.size () >= 2 ? (static_cast<std::uint8_t> (frame[0]) << 8)
                                                    | static_cast<std::uint8_t> (frame[1])
                                                : 0;
    const auto header = frame.size () >= 6 + header_size ? frame.substr (6, header_size) : "";
    if (!require (header.find ("godot-hook") != std::string::npos
                    && header.find ("present") != std::string::npos,
                  "sending hook metadata missing from server-side frame")) {
        return 1;
    }
    raw_client.connect (argv[1]);
    const auto replacement_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (replacement_replies.empty ()
           && std::chrono::steady_clock::now () < replacement_deadline) {
        raw_client.dispatch ();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (!require (replacement_replies.size () == 1 && !replacement_replies[0].succeeded
                    && replacement_replies[0].error
                    && replacement_replies[0].error->code
                         == zlink::stream_connector::error_code_t::disconnected,
                  "replaced connector lost pending request close reply hook")) {
        return 1;
    }
    replacement_hook.unsubscribe ();
    raw_sending_hook.unsubscribe ();
    raw_client.close ();
    inbound.close ();

    stream_connector_t alice;
    stream_connector_t bob;
    std::vector<packet_t> alice_requests;
    std::vector<packet_t> bob_requests;
    std::vector<packet_t> alice_pushes;
    std::vector<packet_t> bob_pushes;
    std::vector<reply_received_context_t> alice_reply_hooks;
    int alice_sending_calls = 0;
    auto alice_sending_hook = alice.on_request_sending (
      [&] (zlink::godot_stream_connector::request_sending_context_t &hook) {
          ++alice_sending_calls;
          hook.set_metadata ("godot-request", "present");
      });
    auto alice_reply_hook = alice.on_reply_received (
      [&] (const reply_received_context_t &hook) { alice_reply_hooks.push_back (hook); });
    alice.on_request_completed (
      [&] (const packet_t &packet) { alice_requests.push_back (packet); });
    bob.on_request_completed ([&] (const packet_t &packet) { bob_requests.push_back (packet); });
    alice.on_packet ([&] (const packet_t &packet) { alice_pushes.push_back (packet); });
    bob.on_packet ([&] (const packet_t &packet) { bob_pushes.push_back (packet); });

    alice.subscribe ("ChatNotify");
    alice.connect (argv[1]);
    bob.connect (argv[1]);

    alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 5.0);
    if (!require (alice_sending_calls == 1 && alice_reply_hooks.empty (),
                  "request hooks ran at the wrong time")) {
        return 1;
    }
    if (!require (until (alice, bob, [&] { return alice_requests.size () == 1; }),
                  "PingReq completion missing")
        || !require (alice_requests[0].name == "PingReq", "PingReq completion name wrong")
        || !require (nlohmann::json::parse (payload (alice_requests[0])).at ("sentAtUnixMs")
                       == "1000",
                     "PingReq response payload wrong")) {
        return 1;
    }
    if (!require (alice_reply_hooks.size () == 1, "reply hook missing after dispatch")
        || !require (alice_reply_hooks[0].request_packet_name == "PingReq"
                       && alice_reply_hooks[0].succeeded && alice_reply_hooks[0].reply
                       && !alice_reply_hooks[0].error && alice_reply_hooks[0].elapsed.count () >= 0,
                     "reply hook context wrong")) {
        return 1;
    }
    alice_sending_hook.unsubscribe ();
    alice_reply_hook.unsubscribe ();

    int canceled_reply_calls = 0;
    auto canceled_reply_hook =
      alice.on_reply_received ([&] (const reply_received_context_t &) { ++canceled_reply_calls; });
    alice.request_json ("JoinReq", R"({"name":"alice"})", 5.0);
    canceled_reply_hook.unsubscribe ();
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
    if (!require (alice_sending_calls == 1 && alice_reply_hooks.size () == 1
                    && canceled_reply_calls == 0,
                  "released request hooks ran again")) {
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
