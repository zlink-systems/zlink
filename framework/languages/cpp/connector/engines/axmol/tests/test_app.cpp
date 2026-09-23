/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>

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
        std::vector<packet_t> alice_replies;
        std::vector<packet_t> bob_replies;
        std::vector<packet_t> alice_pushes;
        std::vector<packet_t> bob_pushes;
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
        check (alice_replies.empty (), "request callback ran before dispatch");
        dispatch_until (alice, bob, [&] { return alice_replies.size () == 1; });
        check (alice_replies[0].name == "PingReq", "request completion lost PingReq name");
        check (nlohmann::json::parse (payload (alice_replies[0])).at ("sentAtUnixMs") == "1000",
               "PingRes payload mismatch");

        alice.request_json ("JoinReq", R"({"name":"alice"})", 10);
        dispatch_until (alice, bob, [&] { return alice_replies.size () == 2; });
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
