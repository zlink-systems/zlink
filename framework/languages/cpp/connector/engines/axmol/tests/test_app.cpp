/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>

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
        std::cerr << "usage: axmol-test-app <engine-lobby-websocket-endpoint>\n";
        return 2;
    }
    try {
        connector_t alice;
        connector_t bob;
        std::optional<request_result_t> alice_ping_reply;
        std::optional<request_result_t> alice_join_reply;
        std::optional<request_result_t> bob_join_reply;
        std::vector<packet_t> alice_pushes;
        std::vector<packet_t> bob_pushes;
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
        check (!alice_ping_reply, "request callback ran before dispatch");
        dispatch_until (alice, bob, [&] { return alice_ping_reply.has_value (); });
        check (alice_ping_reply->reply.has_value (), "PingReq failed");
        check (!alice_ping_reply->error_code && alice_ping_reply->error_message.empty (),
               "successful request carried an error");
        check (nlohmann::json::parse (payload (*alice_ping_reply->reply)).at ("sentAtUnixMs")
                 == "1000",
               "PingRes payload mismatch");

        alice.request_json ("JoinReq", R"({"name":"alice"})", 10,
                            [&] (const request_result_t &result) { alice_join_reply = result; });
        dispatch_until (alice, bob, [&] { return alice_join_reply.has_value (); });
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
        check (bob_pushes.empty (), "unsubscribed Bob received ChatNotify");
        auto bob_chat =
          bob.on ("ChatNotify", [&] (const packet_t &packet) { bob_pushes.push_back (packet); });
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
        check (unrelated_pushes.empty (), "wrong-name push callback ran");
        check (alice_chat.active () && alice_other.active () && bob_chat.active (),
               "subscription handle inactive before unsubscribe");
        bob_chat.unsubscribe ();
        check (!bob_chat.active (), "unsubscribe left handle active");
        alice.send_json ("ChatMsg", R"({"text":"after unsubscribe"})");
        dispatch_until (alice, bob, [&] { return alice_pushes.size () == 3; });
        check (bob_pushes.size () == 1, "unsubscribed callback ran");

        alice.connect (argv[1]);
        check (alice_chat.active (), "subscription lost on reconnect");
        std::optional<request_result_t> rejoined;
        alice.request_json ("JoinReq", R"({"name":"alice"})", 10,
                            [&] (const request_result_t &result) { rejoined = result; });
        dispatch_until (alice, bob, [&] { return rejoined.has_value (); });
        check (rejoined->reply.has_value (), "JoinReq after reconnect failed");
        alice.send_json ("ChatMsg", R"({"text":"after reconnect"})");
        dispatch_until (alice, bob, [&] { return alice_pushes.size () == 4; });
        check (alice_pushes.back ().name == "ChatNotify",
               "named callback did not survive connector recreation");

        connector_t disconnected;
        std::vector<request_result_t> failures;
        disconnected.request_json (
          "PingReq", R"({"sentAtUnixMs":"1000"})", 1,
          [&] (const request_result_t &result) { failures.push_back (result); });
        check (failures.empty (), "failure callback ran before dispatch");
        disconnected.dispatch ();
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
