/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using connector_t = zlink::axmol_stream_connector::stream_connector_t;
using packet_t = zlink::axmol_stream_connector::packet_t;
using reply_context_t = zlink::axmol_stream_connector::reply_received_context_t;

std::string payload (const packet_t &packet)
{
    return std::string (packet.payload.begin (), packet.payload.end ());
}

void check (bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

template <typename Predicate>
void dispatch_until (connector_t &first, connector_t &second, Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        first.dispatch ();
        second.dispatch ();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    check (ready (), "expected adapter callback was not dispatched");
}
} // namespace

int main (int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: axmol-test-app <engine-lobby-websocket-endpoint>\n";
        return 2;
    }
    try {
        zlink::context_t raw_context;
        zlink::stream_socket_t raw_server (raw_context);
        raw_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
        raw_server.options ().notify (false);
        raw_server.bind ("tcp://127.0.0.1:0");
        connector_t raw_client;
        int raw_sending_calls = 0;
        auto raw_hook = raw_client.on_request_sending (
          [&] (zlink::axmol_stream_connector::request_sending_context_t &hook) {
              ++raw_sending_calls;
              check (hook.request_packet_name == "AxmolHookReq" && !hook.actor_id,
                     "raw sending hook context mismatch");
              hook.set_metadata ("axmol-hook", "present");
          });
        std::vector<reply_context_t> replaced_replies;
        auto raw_reply_hook = raw_client.on_reply_received (
          [&] (const reply_context_t &hook) { replaced_replies.push_back (hook); });
        raw_client.connect (raw_server.options ().last_endpoint ());
        raw_client.request_json ("AxmolHookReq", R"({"probe":true})", 10);
        check (raw_sending_calls == 1, "raw sending hook was not synchronous");
        zlink::received_t inbound;
        check (raw_server.recv (inbound) == 0, "raw server missed Axmol hook request");
        const auto frame = inbound.parts ()[0].to_string ();
        const auto header_size = frame.size () >= 2 ? (static_cast<std::uint8_t> (frame[0]) << 8)
                                                        | static_cast<std::uint8_t> (frame[1])
                                                    : 0;
        const auto header = frame.size () >= 6 + header_size ? frame.substr (6, header_size) : "";
        check (header.find ("axmol-hook") != std::string::npos
                 && header.find ("present") != std::string::npos,
               "sending hook metadata missing from server-side frame");
        inbound.close ();
        raw_client.connect (raw_server.options ().last_endpoint ());
        check (replaced_replies.empty (), "reply hook ran before dispatch on connector replacement");
        dispatch_until (raw_client, raw_client, [&] { return replaced_replies.size () == 1; });
        check (replaced_replies[0].request_packet_name == "AxmolHookReq"
                 && !replaced_replies[0].succeeded && !replaced_replies[0].reply
                 && replaced_replies[0].error
                 && replaced_replies[0].error->code
                      == zlink::stream_connector::error_code_t::disconnected,
               "connector replacement did not deliver pending request failure");
        raw_reply_hook.unsubscribe ();
        raw_hook.unsubscribe ();
        raw_client.close ();

        connector_t alice;
        connector_t bob;
        std::vector<packet_t> alice_replies;
        std::vector<packet_t> bob_replies;
        std::vector<packet_t> alice_pushes;
        std::vector<packet_t> bob_pushes;
        std::vector<reply_context_t> reply_hooks;
        std::vector<std::string> sending_names;
        const auto main_thread = std::this_thread::get_id ();
        auto sending_hook = alice.on_request_sending (
          [&] (zlink::axmol_stream_connector::request_sending_context_t &context) {
              check (std::this_thread::get_id () == main_thread,
                     "sending hook did not run in request call");
              sending_names.push_back (context.request_packet_name);
              check (!context.actor_id, "connector request unexpectedly has actor ID");
              context.set_metadata ("axmolHook", "from-client");
          });
        auto reply_hook = alice.on_reply_received ([&] (const reply_context_t &context) {
            check (std::this_thread::get_id () == main_thread,
                   "reply hook did not run on dispatch thread");
            reply_hooks.push_back (context);
        });
        alice.on_request_completed (
          [&] (const packet_t &packet) { alice_replies.push_back (packet); });
        bob.on_request_completed ([&] (const packet_t &packet) { bob_replies.push_back (packet); });
        alice.on_packet ([&] (const packet_t &packet) { alice_pushes.push_back (packet); });
        bob.on_packet ([&] (const packet_t &packet) { bob_pushes.push_back (packet); });
        alice.subscribe ("ChatNotify");
        alice.connect (argv[1]);
        bob.connect (argv[1]);
        check (alice.state () == zlink::axmol_stream_connector::connection_state_t::connected,
               "Alice did not connect");
        check (bob.state () == zlink::axmol_stream_connector::connection_state_t::connected,
               "Bob did not connect");

        alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 10);
        check (sending_names.size () == 1 && sending_names[0] == "PingReq",
               "sending hook was not synchronous");
        check (reply_hooks.empty (), "reply hook ran before dispatch");
        check (alice_replies.empty (), "request callback ran before dispatch");
        dispatch_until (alice, bob,
                        [&] { return alice_replies.size () == 1 && reply_hooks.size () == 1; });
        check (reply_hooks[0].request_packet_name == "PingReq" && reply_hooks[0].succeeded
                 && reply_hooks[0].reply && !reply_hooks[0].error
                 && reply_hooks[0].elapsed >= std::chrono::milliseconds (0),
               "reply hook context mismatch");
        check (!reply_hooks[0].actor_id, "connector reply unexpectedly has actor ID");
        check (alice_replies[0].name == "PingReq", "request completion lost PingReq name");
        check (nlohmann::json::parse (payload (alice_replies[0])).at ("sentAtUnixMs") == "1000",
               "PingRes payload mismatch");

        alice.request_json ("JoinReq", R"({"name":"alice"})", 10);
        check (sending_names.size () == 2 && sending_names[1] == "JoinReq",
               "sending hook missed JoinReq");
        dispatch_until (alice, bob,
                        [&] { return alice_replies.size () == 2 && reply_hooks.size () == 2; });
        check (reply_hooks[1].request_packet_name == "JoinReq" && reply_hooks[1].succeeded,
               "reply hook missed JoinReq");
        sending_hook.unsubscribe ();
        reply_hook.unsubscribe ();
        bob.request_json ("JoinReq", R"({"name":"bob"})", 10);
        dispatch_until (alice, bob, [&] { return bob_replies.size () == 1; });
        check (alice_replies[1].name == "JoinReq" && bob_replies[0].name == "JoinReq",
               "request completion lost JoinReq name");
        const auto alice_join = nlohmann::json::parse (payload (alice_replies[1]));
        const auto bob_join = nlohmann::json::parse (payload (bob_replies[0]));
        check (alice_join.at ("name") == "alice" && bob_join.at ("name") == "bob",
               "JoinRes names mismatch");
        check (!alice_join.at ("actorId").get<std::string> ().empty ()
                 && alice_join.at ("actorId") != bob_join.at ("actorId"),
               "JoinRes actor IDs mismatch");
        alice.request_json ("PingReq", R"({"sentAtUnixMs":"2000"})", 10);
        dispatch_until (alice, bob, [&] { return alice_replies.size () == 3; });
        check (sending_names.size () == 2 && reply_hooks.size () == 2,
               "unsubscribed request hook still ran");

        alice.send_json ("ChatMsg", R"({"text":"unsubscribed"})");
        check (alice_pushes.empty () && bob_pushes.empty (), "push callback ran before dispatch");
        dispatch_until (alice, bob, [&] { return alice_pushes.size () == 1; });
        check (nlohmann::json::parse (payload (alice_pushes[0])).at ("text") == "unsubscribed",
               "first ChatNotify payload mismatch");
        const auto quiet_until =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
        while (std::chrono::steady_clock::now () < quiet_until) {
            alice.dispatch ();
            bob.dispatch ();
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        check (bob_pushes.empty (), "unsubscribed Bob received ChatNotify");
        bob.subscribe ("ChatNotify");
        alice.send_json ("ChatMsg", R"({"text":"hello"})");
        dispatch_until (alice, bob,
                        [&] { return alice_pushes.size () == 2 && bob_pushes.size () == 1; });
        for (const auto *push : {&alice_pushes[1], &bob_pushes[0]}) {
            check (push->name == "ChatNotify", "push lost ChatNotify name");
            const auto notification = nlohmann::json::parse (payload (*push));
            check (notification.at ("actorId") == alice_join.at ("actorId")
                     && notification.at ("name") == "alice" && notification.at ("text") == "hello",
                   "ChatNotify payload mismatch");
        }
        alice.close ();
        bob.close ();
        std::cout << "axmol-engine-lobby=ok\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
