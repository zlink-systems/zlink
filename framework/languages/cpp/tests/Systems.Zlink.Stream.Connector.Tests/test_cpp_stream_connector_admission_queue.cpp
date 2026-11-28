/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_connector_request_timing.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_wire_frames.hpp"

#include <fstream>
#include <future>
#include <iostream>
#include <zlink/json.hpp>

namespace
{
namespace sc = zlink::stream_connector;
using namespace stream_connector_fixture;
using namespace std::chrono_literals;
using json = nlohmann::json;
using ms = std::chrono::milliseconds;
using kind = sc::message_kind_t;
using error = sc::error_code_t;
using reply_t = sc::result_t<std::vector<std::uint8_t>>;

constexpr std::string_view enqueue = "$zlink.admission.enqueue";
constexpr std::string_view position = "$zlink.admission.position";
constexpr std::string_view ticket = "$zlink.admission.ticket";
constexpr char required[] = "$zlink.admission.queueRequired";
constexpr char retry_after[] = "$zlink.admission.retryAfterMs";

void require (bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

sc::connector_options_t options (const json &given, sc::dispatch_mode_t mode)
{
    sc::connector_options_t value;
    value.endpoint = "tcp://127.0.0.1:1";
    value.dispatch_mode = mode;
    value.heartbeat.enabled = false;
    value.reconnect.enabled = false;
    value.request_timeout = ms (given.value ("originalDeadlineMs", 25000));
    value.max_admission_wait = ms (given.value ("maxAdmissionWaitMs", 25000));
    return value;
}

struct peer_t
{
    sc::connector_t connector;
    std::shared_ptr<sc::detail::connector_state_t> state;
    std::shared_ptr<fed_read_connection_t> transport = std::make_shared<fed_read_connection_t> ();
    scripted_request_timing_t &clock;
    std::map<std::string, std::optional<reply_t>, std::less<>> results;
    std::map<std::string, std::vector<server_frame_t>, std::less<>> sent;
    std::vector<std::string> callbacks;
    std::vector<json> positions;
    std::vector<sc::subscription_t> subscriptions;
    std::size_t read_count = 0;
    int queued = 0;
    int errors = 0;
    std::string wire_ticket;

    peer_t (const json &given,
            std::string value,
            sc::dispatch_mode_t mode = sc::dispatch_mode_t::manual) :
        connector (sc::connector_factory_t::create (options (given, mode))),
        state (std::static_pointer_cast<sc::detail::connector_state_t> (
          connector_internal_handle (connector))),
        clock (install_clock ()),
        wire_ticket (std::move (value))
    {
        subscribe_queue (connector);
        subscriptions.push_back (connector.on_admission_waiting (
          [this] (const auto &) { callbacks.push_back ("OnAdmissionWaiting"); }));
        subscriptions.push_back (connector.on_error ([this] (const auto &) { ++errors; }));
        state->connection = transport;
        sc::detail::change_state (state, sc::connection_state_t::connected);
        sc::detail::start_read_loop (state);
    }

    ~peer_t ()
    {
        (void) connector.close ();
        (void) connector.dispatch ();
    }

    scripted_request_timing_t &install_clock ()
    {
        auto timing = std::make_unique<scripted_request_timing_t> ();
        auto &installed = *timing;
        state->request_timing = std::move (timing);
        return installed;
    }

    void subscribe_queue (sc::connector_t &client)
    {
        subscriptions.push_back (client.on_admission_queued ([this] {
            ++queued;
            callbacks.push_back ("OnAdmissionQueued");
        }));
        subscriptions.push_back (client.on_admission_position ([this] (auto order, auto wait) {
            callbacks.push_back ("OnAdmissionPosition");
            positions.push_back ({{"position", order}, {"estimatedWaitMs", wait.count ()}});
        }));
    }

    void collect_writes ()
    {
        // The write notification precedes posting its completion to the strand.
        for (int i = 0; i < 2; ++i) {
            auto barrier = std::make_shared<std::promise<void>> ();
            auto done = barrier->get_future ();
            boost::asio::post (state->write_strand, [barrier] { barrier->set_value (); });
            require (done.wait_for (5s) == std::future_status::ready, "write strand did not drain");
        }
        const auto count = transport->write_count ();
        while (read_count < count) {
            auto bytes = transport->wait_for_written (read_count++);
            require (bytes.has_value (), "expected a write on the test transport");
            auto wire = as_string (*bytes);
            const auto offset = wire.find (enqueue);
            if (offset != std::string::npos) {
                // The connector decoder accepts server-to-client reserved names.
                wire[offset] = 'x';
            }
            auto frame = try_read_server_frame (wire);
            require (frame.has_value () && wire.empty (), "expected one complete outbound frame");
            if (offset != std::string::npos) {
                frame->header.name = enqueue;
                require (frame->header.kind == kind::request
                           && frame->header.codec == sc::codec_t::json,
                         "enqueue was not a JSON request");
                require (json::parse (frame->payload) == json::object (),
                         "enqueue payload differs");
            }
            sent[frame->header.name].push_back (std::move (*frame));
        }
    }

    void expect_writes (std::size_t count)
    {
        require (count == 0 || transport->wait_for_written (count - 1).has_value (),
                 "expected outbound frame was not sent");
        collect_writes ();
    }

    void dispatch ()
    {
        while (connector.pending_dispatch_count () != 0) {
            require (static_cast<bool> (connector.dispatch ()), "callback dispatch failed");
        }
    }

    void request (const std::string &name)
    {
        results.emplace (name, std::nullopt);
        sc::metadata_t metadata;
        metadata.with ("app", name);
        connector.request (sc::packet_t{name, std::move (metadata), sc::codec_t::raw, false, {1}})
          .submit<std::vector<std::uint8_t>> ([this, name] (auto reply) {
              require (!results.at (name), "request completed twice");
              if (reply) {
                  callbacks.push_back ("requestComplete");
              }
              results.at (name) = std::move (reply);
          });
        expect_writes (read_count + 1);
    }

    void reply (kind type, std::string_view name, const json &payload, sc::metadata_t metadata = {})
    {
        const auto &attempts = sent[std::string (name)];
        require (!attempts.empty (), "expected request sequence was not written");
        transport->feed (make_server_frame (type, *attempts.back ().header.request_seq, {},
                                            payload.dump (), false, std::nullopt,
                                            std::move (metadata)));
    }

    void reject (std::string_view name, std::string code, sc::metadata_t metadata)
    {
        reply (kind::error, name, {{"code", std::move (code)}, {"message", "admission"}},
               std::move (metadata));
    }

    void queue (std::string_view name)
    {
        reject (name, "unavailable", sc::metadata_t{}.with (required, "1"));
        dispatch ();
        require (!results.at (std::string (name)), "queueRequired completed the request");
    }

    std::vector<std::uint8_t> push_frame (std::string name, const json &payload)
    {
        auto placeholder = name;
        placeholder.front () = 'x';
        auto wire = make_server_frame (kind::send, 0, placeholder, payload.dump ()).to_string ();
        const auto offset = wire.find (placeholder);
        require (offset != std::string::npos, "expected Send packet name in frame");
        wire[offset] = name.front ();
        return as_bytes (wire);
    }

    void push (std::string name, const json &payload)
    {
        transport->feed (push_frame (std::move (name), payload));
    }

    void prime_ticket (const std::string &name, const std::string &old)
    {
        queue (name);
        expect_writes (read_count + 1);
        reply (kind::response, enqueue, {{"ticket", old}, {"expiresInMs", 30000}});
        clock.advance (0ms);
        expect_writes (read_count + 1);
        dispatch ();
        require (sent.at (name).back ().header.metadata.values.at (std::string (ticket)) == old,
                 "initial replay did not carry the old ticket");
        sent.erase (std::string (enqueue));
        sent.at (name).erase (sent.at (name).begin ());
        queued = 0;
    }

    void step (const json &step)
    {
        callbacks.clear ();
        positions.clear ();
        const auto &expected = step.at ("expect");
        const auto count = read_count;
        const auto count_before = sent[std::string (enqueue)].size ();
        if (expected.contains ("deadlineMs")) {
            clock.advance (ms (expected.at ("deadlineMs").get<int> () - 1));
            dispatch ();
            require (!results.at ("a"), "request timed out early");
        }
        if (step.contains ("atMs")) {
            clock.advance (ms (step.at ("atMs").get<int> ()));
        }
        const auto action = step.at ("action").get<std::string> ();
        const auto name = step.value ("request", std::string ("a"));
        const auto sequence = [&] {
            return json{{"position", step.at ("position")},
                        {"estimatedWaitMs", step.at ("estimatedWaitMs")}};
        };
        if (action == "queueRequired") {
            queue (name);
        } else if (action == "positionResponse") {
            reply (kind::response, enqueue, sequence ());
        } else if (action == "positionSend") {
            push (std::string (position), sequence ());
        } else if (action == "ticketSend") {
            require (step.at ("ticket") == "T", "unknown fixture ticket label");
            push (std::string (ticket),
                  {{"ticket", wire_ticket}, {"expiresInMs", step.at ("expiresInMs")}});
            clock.advance (std::chrono::duration_cast<ms> (clock.now ().time_since_epoch ()));
        } else if (action == "stageOneReject") {
            const auto delay = step.at ("retryAfterMs").get<std::string> ();
            clock.expect_jitter (0ms);
            reject (name, "unavailable", sc::metadata_t{}.with (retry_after, delay));
            clock.advance (std::chrono::duration_cast<ms> (clock.now ().time_since_epoch ())
                           + ms (std::stoi (delay)));
            expect_writes (count + 1);
            require (sent.at (name).back ().header.metadata.values.at (std::string (ticket))
                       == wire_ticket,
                     "stage one retry did not retain the ticket");
        } else if (action == "complete") {
            reply (kind::response, name, json::object ());
        } else if (action == "error") {
            sc::metadata_t metadata;
            for (const auto &[key, value] : step.at ("metadata").items ()) {
                metadata.with (key, value.get<std::string> ());
            }
            if (metadata.values.contains (retry_after)) {
                clock.expect_jitter (0ms);
            }
            reject (name, step.at ("code").get<std::string> (), std::move (metadata));
        } else if (action == "enqueueResponse" || action == "send") {
            for (const auto &[waiting, _] : results) {
                queue (waiting);
            }
            if (!results.empty ()) {
                expect_writes (count + 1);
            }
            if (action == "enqueueResponse") {
                reply (kind::response, enqueue, step.at ("payload"));
            } else {
                push (step.at ("packet").get<std::string> (), step.at ("payload"));
            }
        } else {
            require (action == "silence" || action == "wait", "unknown fixture action");
        }
        if (expected.contains ("enqueueCount")) {
            const auto wanted = expected.at ("enqueueCount").get<std::size_t> ();
            dispatch ();
            require (results.empty () || !results.at (name), "queue rejection completed a request");
            expect_writes (count + wanted - count_before);
            require (sent.at (std::string (enqueue)).size () == wanted, "enqueue count differs");
        }
        if (expected.contains ("retriedRequests")) {
            expect_writes (count + expected.at ("retriedRequests").size ());
            for (const auto &retry : expected.at ("retriedRequests")) {
                const auto &attempts = sent.at (retry.get<std::string> ());
                require (attempts.size () == 2, "ticket did not release exactly one retry");
                const auto &first = attempts.front ();
                const auto &second = attempts.back ();
                require (second.header.metadata.values.at (std::string (ticket)) == wire_ticket,
                         "retry did not carry the new ticket");
                require (first.header.metadata.values.at ("app")
                             == second.header.metadata.values.at ("app")
                           && first.header.name == second.header.name
                           && first.payload == second.payload
                           && first.header.correlation_id == second.header.correlation_id
                           && first.header.request_seq != second.header.request_seq,
                         "retry changed the logical request");
            }
        }
        dispatch ();
        if (expected.contains ("callbacks")) {
            require (json (callbacks) == expected.at ("callbacks"), "callback order differs");
        }
        if (!positions.empty ()) {
            require (positions == std::vector<json>{sequence ()}, "position arguments differ");
        }
        if (expected.contains ("positionCallbacks")) {
            require (positions.size () == expected.at ("positionCallbacks"),
                     "late position reached callback");
        }
        if (expected.contains ("queuedCallbacks")) {
            require (queued == expected.at ("queuedCallbacks"), "queued callback count differs");
        }
        if (expected.contains ("cachedTicket")) {
            require (expected.at ("cachedTicket").is_null () && sent.at (name).size () == 1
                       && !sent.at (std::string (enqueue))
                             .back ()
                             .header.metadata.values.contains (std::string (ticket)),
                     "queue re-entry sent the old ticket");
        }
        if (expected.contains ("connectionClosed")) {
            require (connector.is_connected () != expected.at ("connectionClosed").get<bool> (),
                     "connection state differs");
        }
        if (expected.value ("discarded", false)) {
            require (callbacks.empty () && errors == 0
                       && connector.received_count (step.at ("packet").get<std::string> ()) == 0,
                     "unsolicited reserved Send reached the application");
        }
        for (const auto &[request_name, result] : results) {
            const auto terminal = expected.value (request_name, std::string ());
            if (terminal == "FrameDecodeFailed") {
                require (result && result->error_code () == error::frame_decode_failed,
                         "expected FrameDecodeFailed");
            } else if (terminal == "pending") {
                require (!result, "unrelated request completed");
            }
        }
        if (expected.contains ("result")) {
            require (expected.at ("result") == "RequestTimeout" && results.at ("a")
                       && results.at ("a")->error_code () == error::request_timeout,
                     "expected RequestTimeout");
            const auto writes = transport->write_count ();
            clock.advance (ms (expected.at ("deadlineMs").get<int> ()) + 15s);
            collect_writes ();
            require ((transport->write_count () != writes) == expected.at ("sendAfterDeadline"),
                     "send occurred after request deadline");
        }
    }
};
void malformed_response_and_ticket_in_one_read (const std::string &wire_ticket)
{
    peer_t peer (json::object (), wire_ticket, sc::dispatch_mode_t::immediate);
    peer.request ("a");
    peer.request ("b");
    peer.queue ("a");
    peer.expect_writes (3);
    peer.queue ("b");

    const auto seq = peer.sent.at (std::string (enqueue)).back ().header.request_seq;
    const auto response = make_server_frame (kind::response, *seq, {}, R"({"position":"bad"})");
    auto batch = as_bytes (response.to_string ());
    const auto push =
      peer.push_frame (std::string (ticket), {{"ticket", wire_ticket}, {"expiresInMs", 30000}});
    batch.insert (batch.end (), push.begin (), push.end ());
    peer.transport->feed (std::move (batch));
    peer.clock.advance (0ms);
    peer.expect_writes (4);
    peer.dispatch ();
    require (peer.results.at ("a")
               && peer.results.at ("a")->error_code () == error::frame_decode_failed,
             "malformed enqueue response did not fail its initiator");
    require (peer.sent.at ("a").size () == 1 && peer.sent.at ("b").size () == 2
               && peer.sent.at ("b").back ().header.metadata.values.at (std::string (ticket))
                    == wire_ticket
               && peer.connector.is_connected (),
             "ticket did not release only the other waiting request");
}

} // namespace

int main ()
{
    std::ifstream input (ZLINK_ADMISSION_QUEUE_FIXTURE_PATH);
    const auto fixture = json::parse (input);
    const auto &scenarios = fixture.at ("connectorScenarios");
    require (scenarios.size () == 11, "unexpected fixture scenario count");
    const auto &tickets = fixture.at ("ticketVectors");
    const auto wire_ticket = tickets.at (0).at ("ticket").get<std::string> ();
    int passed = 0;
    for (const auto &scenario : scenarios) {
        const auto name = scenario.at ("name").get<std::string> ();
        try {
            const auto &given = scenario.at ("given");
            peer_t peer (given, wire_ticket);
            if (given.contains ("requests")) {
                for (const auto &request : given.at ("requests")) {
                    peer.request (request.get<std::string> ());
                }
            } else {
                peer.request ("a");
                peer.clock.advance (ms (given.at ("firstRejectionMs").get<int> ()));
                peer.queue ("a");
                peer.expect_writes (2);
            }
            if (given.contains ("cachedTicket")) {
                require (given.at ("cachedTicket") == "old", "unknown cached ticket label");
                peer.prime_ticket ("a", tickets.at (8).at ("ticket").get<std::string> ());
            }
            for (const auto &step : scenario.at ("steps")) {
                peer.step (step);
            }
            ++passed;
            std::cout << name << ": PASS\n";
        }
        catch (const std::exception &failure) {
            std::cerr << name << ": FAIL: " << failure.what () << '\n';
        }
    }
    std::cout << passed << '/' << scenarios.size () << " connector scenarios passed\n";
    try {
        malformed_response_and_ticket_in_one_read (wire_ticket);
        std::cout << "malformed-response-and-ticket-in-one-read: PASS\n";
    }
    catch (const std::exception &failure) {
        std::cerr << "malformed-response-and-ticket-in-one-read: FAIL: " << failure.what () << '\n';
        return 1;
    }
    return passed == static_cast<int> (scenarios.size ()) ? 0 : 1;
}
