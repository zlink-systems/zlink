/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using connector_t = zlink::axmol_stream_connector::stream_connector_t;
using packet_t = zlink::axmol_stream_connector::packet_t;
using reply_context_t = zlink::axmol_stream_connector::reply_received_context_t;
using request_result_t = zlink::axmol_stream_connector::request_result_t;
using error_code_t = zlink::axmol_stream_connector::error_code_t;

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
        std::cerr << "engine-required: axmol-test-app <engine-lobby-websocket-endpoint>\n";
        return 2;
    }
    try {
        zlink::context_t raw_context;
        zlink::stream_socket_t raw_server (raw_context);
        raw_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
        raw_server.options ().notify (false);
        raw_server.bind ("tcp://127.0.0.1:0");
        connector_t raw_client;
        std::vector<reply_context_t> replaced_replies;
        std::vector<request_result_t> replaced_completions;
        auto raw_reply_hook = raw_client.on_reply_received (
          [&] (const reply_context_t &context) { replaced_replies.push_back (context); });
        raw_client.connect (raw_server.options ().last_endpoint ());
        raw_client.request_json (
          "AxmolPendingReq", R"({"probe":true})", 10,
          [&] (const request_result_t &result) { replaced_completions.push_back (result); });
        zlink::received_t inbound;
        check (raw_server.recv (inbound) == 0, "raw server missed pending Axmol request");
        inbound.close ();
        raw_client.connect (raw_server.options ().last_endpoint ());
        check (replaced_replies.empty () && replaced_completions.empty (),
               "replacement callbacks ran before adapter dispatch");
        dispatch_until (raw_client, raw_client, [&] {
            return replaced_replies.size () == 1 && replaced_completions.size () == 1;
        });
        check (!replaced_replies[0].succeeded && replaced_replies[0].error
                 && replaced_replies[0].error->code == error_code_t::disconnected
                 && replaced_completions[0].error_code == error_code_t::disconnected,
               "pending Axmol request lost reconnect failure");
        raw_client.close ();

        connector_t alice;
        connector_t bob;
        std::optional<request_result_t> alice_ping_reply;
        std::optional<request_result_t> alice_join_reply;
        std::optional<request_result_t> bob_join_reply;
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
              context.set_metadata ("axmolHook", "from-client");
          });
        auto reply_hook = alice.on_reply_received ([&] (const reply_context_t &context) {
            check (std::this_thread::get_id () == main_thread,
                   "reply hook did not run on dispatch thread");
            reply_hooks.push_back (context);
        });
        auto throwing_sending_hook = alice.on_request_sending (
          [] (zlink::axmol_stream_connector::request_sending_context_t &) {
              throw std::runtime_error ("expected Axmol sending hook failure");
          });
        auto throwing_reply_hook = alice.on_reply_received ([] (const reply_context_t &) {
            throw std::runtime_error ("expected Axmol reply hook failure");
        });
        std::vector<packet_t> unrelated_pushes;
        auto alice_chat = alice.on (
          "ChatNotify", [&] (const packet_t &packet) { alice_pushes.push_back (packet); });
        auto alice_other = alice.on (
          "OtherNotify", [&] (const packet_t &packet) { unrelated_pushes.push_back (packet); });
        alice.connect (argv[1]);
        bob.connect (argv[1]);
        check (alice.state () == zlink::axmol_stream_connector::connection_state_t::connected,
               "Alice did not connect");
        check (bob.state () == zlink::axmol_stream_connector::connection_state_t::connected,
               "Bob did not connect");

        alice.request_json ("PingReq", R"({"sentAtUnixMs":"1000"})", 10,
                            [&] (const request_result_t &result) { alice_ping_reply = result; });
        check (sending_names.size () == 1 && sending_names[0] == "PingReq",
               "sending hook was not synchronous");
        check (reply_hooks.empty (), "reply hook ran before dispatch");
        check (!alice_ping_reply, "request callback ran before dispatch");
        dispatch_until (alice, bob,
                        [&] { return alice_ping_reply.has_value () && reply_hooks.size () == 1; });
        check (reply_hooks[0].request_packet_name == "PingReq" && reply_hooks[0].succeeded
                 && reply_hooks[0].reply && !reply_hooks[0].error,
               "reply hook context mismatch");
        throwing_sending_hook.unsubscribe ();
        throwing_reply_hook.unsubscribe ();
        check (alice_ping_reply->reply.has_value (), "PingReq failed");
        check (!alice_ping_reply->error_code && alice_ping_reply->error_message.empty (),
               "successful request carried an error");
        check (nlohmann::json::parse (payload (*alice_ping_reply->reply)).at ("sentAtUnixMs")
                 == "1000",
               "PingRes payload mismatch");

        alice.request_json ("JoinReq", R"({"name":"alice"})", 10,
                            [&] (const request_result_t &result) { alice_join_reply = result; });
        check (sending_names.size () == 2 && sending_names[1] == "JoinReq",
               "sending hook missed JoinReq");
        dispatch_until (alice, bob,
                        [&] { return alice_join_reply.has_value () && reply_hooks.size () == 2; });
        sending_hook.unsubscribe ();
        reply_hook.unsubscribe ();
        bob.request_json ("JoinReq", R"({"name":"bob"})", 10,
                          [&] (const request_result_t &result) { bob_join_reply = result; });
        dispatch_until (alice, bob, [&] { return bob_join_reply.has_value (); });
        check (alice_join_reply->reply.has_value () && bob_join_reply->reply.has_value (),
               "JoinReq failed");
        const auto alice_join = nlohmann::json::parse (payload (*alice_join_reply->reply));
        const auto bob_join = nlohmann::json::parse (payload (*bob_join_reply->reply));
        check (alice_ping_reply->reply && alice_join_reply->reply && bob_join_reply->reply,
               "per-call request callback crossed replies");
        check (alice_join.at ("name") == "alice" && bob_join.at ("name") == "bob",
               "JoinRes names mismatch");
        check (!alice_join.at ("actorId").get<std::string> ().empty ()
                 && alice_join.at ("actorId") != bob_join.at ("actorId"),
               "JoinRes actor IDs mismatch");

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
        check (bob_pushes.empty (), "Bob without a handler received ChatNotify");
        /* stream-connector §10: a push no handler took stays queued until a handler
         * or a wait surface takes it, so the handler Bob registers now also
         * receives the push that arrived while he had none. */
        auto bob_chat =
          bob.on ("ChatNotify", [&] (const packet_t &packet) { bob_pushes.push_back (packet); });
        alice.send_json ("ChatMsg", R"({"text":"hello"})");
        dispatch_until (alice, bob,
                        [&] { return alice_pushes.size () == 2 && bob_pushes.size () == 2; });
        check (nlohmann::json::parse (payload (bob_pushes[0])).at ("text") == "unsubscribed",
               "queued ChatNotify did not reach the later handler");
        for (const auto *push : {&alice_pushes[1], &bob_pushes[1]}) {
            check (push->name == "ChatNotify", "push lost ChatNotify name");
            const auto notification = nlohmann::json::parse (payload (*push));
            check (notification.at ("actorId") == alice_join.at ("actorId")
                     && notification.at ("name") == "alice" && notification.at ("text") == "hello",
                   "ChatNotify payload mismatch");
        }
        check (unrelated_pushes.empty (), "wrong-name push callback ran");
        check (alice_chat.active () && alice_other.active () && bob_chat.active (),
               "subscription handle inactive before unsubscribe");
        bob_chat.unsubscribe ();
        check (!bob_chat.active (), "unsubscribe left handle active");
        alice.send_json ("ChatMsg", R"({"text":"after unsubscribe"})");
        dispatch_until (alice, bob, [&] { return alice_pushes.size () == 3; });
        check (bob_pushes.size () == 2, "unsubscribed callback ran");

        std::vector<reply_context_t> rejoin_hooks;
        auto rejoin_hook = alice.on_reply_received (
          [&] (const reply_context_t &context) { rejoin_hooks.push_back (context); });
        alice.connect (argv[1]);
        check (alice_chat.active (), "subscription lost on reconnect");
        std::optional<request_result_t> rejoined;
        alice.request_json ("JoinReq", R"({"name":"alice"})", 10,
                            [&] (const request_result_t &result) { rejoined = result; });
        dispatch_until (alice, bob,
                        [&] { return rejoined.has_value () && rejoin_hooks.size () == 1; });
        check (rejoined->reply.has_value (), "JoinReq after reconnect failed");
        check (rejoin_hooks[0].request_packet_name == "JoinReq" && rejoin_hooks[0].succeeded,
               "reply hook did not survive reconnect");
        alice.send_json ("ChatMsg", R"({"text":"after reconnect"})");
        dispatch_until (alice, bob, [&] { return alice_pushes.size () == 4; });
        check (alice_pushes.back ().name == "ChatNotify",
               "named callback did not survive connector recreation");

        connector_t disconnected;
        std::vector<request_result_t> failures;
        std::vector<reply_context_t> failed_reply_hooks;
        auto failure_hook = disconnected.on_reply_received (
          [&] (const reply_context_t &context) { failed_reply_hooks.push_back (context); });
        disconnected.request_json (
          "PingReq", R"({"sentAtUnixMs":"1000"})", 1,
          [&] (const request_result_t &result) { failures.push_back (result); });
        check (failures.empty (), "failure callback ran before dispatch");
        disconnected.dispatch ();
        check (failed_reply_hooks.size () == 1 && !failed_reply_hooks[0].succeeded
                 && failed_reply_hooks[0].error
                 && failed_reply_hooks[0].error->code == error_code_t::disconnected,
               "failed request reply hook missing error");
        check (failures.size () == 1 && !failures[0].reply
                 && failures[0].error_code == error_code_t::disconnected
                 && !failures[0].error_message.empty (),
               "failed request callback missing error");
        disconnected.close ();
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
