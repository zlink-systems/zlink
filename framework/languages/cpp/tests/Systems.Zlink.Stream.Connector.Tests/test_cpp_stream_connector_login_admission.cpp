/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_connector_cases.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_wire_frames.hpp"
#include "runtime/connector_runtime.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <zlink/json.hpp>
#include <zlink/stream_e2e_client.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace
{
namespace sc = zlink::stream_connector;
using namespace stream_connector_fixture;
using namespace std::chrono_literals;
using json = nlohmann::json;
using clock_t = sc::detail::request_clock_t;
using elapsed_t = std::chrono::milliseconds;
using payload_t = std::vector<std::uint8_t>;
using completion_t = sc::result_t<payload_t>;

void require (bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

class scripted_request_timing_t final : public sc::detail::request_timing_t
{
  public:
    clock_t::time_point now () const override { return clock_t::time_point{} + _elapsed.load (); }

    elapsed_t admission_jitter () override
    {
        require (!_jitter.empty (), "runtime requested unexpected admission jitter");
        const auto delay = _jitter.front ();
        _jitter.pop_front ();
        return delay;
    }

    std::function<void ()> schedule (clock_t::time_point when,
                                     std::function<void ()> operation) override
    {
        std::lock_guard lock (_mutex);
        const auto key = std::make_pair (when, _next_timer++);
        _timers.emplace (key, std::move (operation));
        return [this, key] {
            std::lock_guard cancel_lock (_mutex);
            _timers.erase (key);
        };
    }

    void advance (elapsed_t elapsed)
    {
        require (elapsed >= _elapsed.load (), "test clock moved backwards");
        _elapsed = elapsed;
        for (;;) {
            std::function<void ()> due;
            {
                std::lock_guard lock (_mutex);
                const auto first = _timers.begin ();
                if (first == _timers.end () || first->first.first > now ()) {
                    return;
                }
                due = std::move (first->second);
                _timers.erase (first);
            }
            due ();
        }
    }

    void expect_jitter (elapsed_t delay) { _jitter.push_back (delay); }
    bool jitter_consumed () const { return _jitter.empty (); }

  private:
    std::atomic<elapsed_t> _elapsed{0ms};
    std::deque<elapsed_t> _jitter;
    std::mutex _mutex;
    std::map<std::pair<clock_t::time_point, std::size_t>, std::function<void ()>> _timers;
    std::size_t _next_timer = 0;
};

struct harness_t
{
    sc::connector_t connector;
    std::shared_ptr<sc::detail::connector_state_t> state;
    std::shared_ptr<fed_read_connection_t> transport = std::make_shared<fed_read_connection_t> ();
    scripted_request_timing_t *timing = nullptr;
    std::optional<completion_t> result;
    std::vector<elapsed_t> waiting_delays;
    int request_callbacks = 0;
    int sending_hooks = 0;
    int reply_hooks = 0;
    int stream_errors = 0;
    std::vector<sc::subscription_t> subscriptions;

    explicit harness_t (sc::connector_options_t options) :
        connector (sc::connector_factory_t::create (std::move (options))),
        state (std::static_pointer_cast<sc::detail::connector_state_t> (
          connector_internal_handle (connector)))
    {
        auto clock = std::make_unique<scripted_request_timing_t> ();
        timing = clock.get ();
        state->request_timing = std::move (clock);
        state->connection = transport;
        sc::detail::change_state (state, sc::connection_state_t::connected);
        sc::detail::start_read_loop (state);

        subscriptions.push_back (
          connector.on_admission_waiting ([this] (const sc::admission_waiting_context_t &context) {
              waiting_delays.push_back (context.retry_after);
          }));
        subscriptions.push_back (
          connector.on_request_sending ([this] (auto &) { ++sending_hooks; }));
        subscriptions.push_back (
          connector.on_reply_received ([this] (const auto &) { ++reply_hooks; }));
        subscriptions.push_back (connector.on_error ([this] (const auto &) { ++stream_errors; }));
    }

    ~harness_t ()
    {
        (void) connector.close ();
        (void) connector.dispatch ();
    }

    void submit (sc::packet_t packet)
    {
        connector.request (std::move (packet)).submit<payload_t> ([this] (completion_t reply) {
            ++request_callbacks;
            result = std::move (reply);
        });
    }

    void drain_writes () const
    {
        auto barrier = std::make_shared<std::promise<void>> ();
        auto completion = barrier->get_future ();
        boost::asio::post (state->write_strand, [barrier] { barrier->set_value (); });
        require (completion.wait_for (5s) == std::future_status::ready,
                 "write strand did not reach the test barrier");
    }

    server_frame_t read_write (std::size_t index)
    {
        auto bytes = transport->wait_for_written (index);
        require (bytes.has_value (), "expected a frame on the same transport");
        auto wire = as_string (*bytes);
        auto frame = try_read_server_frame (wire);
        require (frame.has_value () && wire.empty (), "outbound write was not one complete frame");
        return std::move (*frame);
    }

    void dispatch ()
    {
        while (connector.pending_dispatch_count () != 0) {
            require (static_cast<bool> (connector.dispatch ()), "callback dispatch failed");
        }
    }
};

sc::connector_options_t options_for (const json &given)
{
    sc::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:1";
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    options.request_timeout = elapsed_t (given.at ("requestTimeoutMs").get<std::int64_t> ());
    if (!given.at ("maxAdmissionWaitMs").is_null ()) {
        options.max_admission_wait =
          elapsed_t (given.at ("maxAdmissionWaitMs").get<std::int64_t> ());
    }
    return options;
}

payload_t fixture_payload (const std::string &base64)
{
    namespace codec = boost::beast::detail::base64;
    payload_t result (codec::decoded_size (base64.size ()));
    const auto decoded = codec::decode (result.data (), base64.data (), base64.size ());
    result.resize (decoded.first);
    return result;
}

void feed_response (harness_t &harness, const json &response, const json &contract)
{
    const auto seq = std::stoull (response.at ("requestSeq").get<std::string> ());
    if (response.at ("kind") == "Error") {
        auto body = contract.at ("errorBody");
        body["code"] = response.at ("remoteCode");
        sc::metadata_t metadata;
        if (!response.at ("retryAfterMs").is_null ()) {
            metadata.with (contract.at ("retryMetadataKey").get<std::string> (),
                           response.at ("retryAfterMs").get<std::string> ());
        }
        harness.transport->feed (make_server_frame (sc::message_kind_t::error, seq, {},
                                                    body.dump (), false, std::nullopt,
                                                    std::move (metadata)));
    } else {
        require (response.at ("kind") == "Response", "unknown stage 1 fixture response kind");
        harness.transport->feed (make_server_frame (sc::message_kind_t::response, seq, {}, "ok"));
    }
}

void verify_outcome (const completion_t &result, const std::string &outcome)
{
    using error = sc::error_code_t;
    if (outcome == "success") {
        require (result && as_string (result.value ()) == "ok", "request did not return success");
        return;
    }
    const std::map<std::string, error> codes{{"FrameDecodeFailed", error::frame_decode_failed},
                                             {"RemoteError", error::remote_error},
                                             {"RequestTimeout", error::request_timeout}};
    require (result.error_code () == codes.at (outcome), "unexpected request terminal result");
}

void run_scenario (const json &scenario, const json &contract, bool compressed = false)
{
    const auto &given = scenario.at ("given");
    const auto &expect = scenario.at ("expect");
    const auto options = options_for (given);
    harness_t harness (options);
    sc::packet_t packet;
    packet.name = given.at ("packetName").get<std::string> ();
    packet.compressed = compressed;
    packet.payload = fixture_payload (given.at ("payloadBase64").get<std::string> ());
    packet.metadata.values =
      given.at ("applicationMetadata").get<std::map<std::string, std::string>> ();

    const auto send_times = expect.at ("sendAtElapsedMs").get<std::vector<std::int64_t>> ();
    const auto sequences = expect.at ("requestSeqs").get<std::vector<std::string>> ();
    const auto &responses = given.at ("responses");
    const auto terminal = expect.value ("terminalAtElapsedMs",
                                        responses.back ().at ("atElapsedMs").get<std::int64_t> ());
    std::set<std::int64_t> events{terminal};
    for (const auto send : send_times) {
        events.insert (send);
    }
    for (const auto &response : responses) {
        events.insert (response.at ("atElapsedMs").get<std::int64_t> ());
    }
    const auto ticks = events;
    for (const auto tick : ticks) {
        if (tick > 0) {
            events.insert (tick - 1);
        }
    }

    int remaining_waits = expect.at ("waitingCallbacks").get<int> ();
    std::vector<elapsed_t> delays;
    for (const auto &response : responses) {
        if (response.at ("kind") == "Error" && remaining_waits > 0
            && !response.at ("retryAfterMs").is_null ()) {
            harness.timing->expect_jitter (elapsed_t (response.at ("jitterMs").get<int> ()));
            delays.emplace_back (std::stoll (response.at ("retryAfterMs").get<std::string> ()));
            --remaining_waits;
        }
    }

    harness.submit (packet);
    std::size_t sent = 0;
    std::string first_correlation;
    for (const auto tick : events) {
        harness.timing->advance (elapsed_t (tick));
        if (sent < send_times.size () && tick == send_times[sent]) {
            auto frame = harness.read_write (sent);
            require (frame.header.kind == sc::message_kind_t::request
                       && frame.header.request_seq == std::stoull (sequences.at (sent)),
                     "wrong kind or request sequence");
            require (frame.header.name == packet.name, "retry changed the request packet name");
            require (frame.compressed == compressed, "retry changed the compression flag");
            require (!expect.at ("payloadPreserved").get<bool> ()
                       || frame.payload == as_string (packet.payload),
                     "retry changed the payload");
            require (!expect.at ("applicationMetadataPreserved").get<bool> ()
                       || frame.header.metadata.values == packet.metadata.values,
                     "retry changed the application metadata");
            if (sent == 0) {
                // Connector assigns the initial wire correlation. Preserve that exact value.
                first_correlation = frame.header.correlation_id;
                require (!first_correlation.empty (), "request omitted its correlation");
            }
            require (!expect.at ("correlationPreserved").get<bool> ()
                       || frame.header.correlation_id == first_correlation,
                     "retry changed the logical correlation");
            ++sent;
        }
        harness.drain_writes ();
        require (harness.transport->write_count () == sent, "unexpected write time");
        for (const auto &response : responses) {
            if (response.at ("atElapsedMs") == tick) {
                feed_response (harness, response, contract);
            }
        }
        harness.dispatch ();
        require (harness.result.has_value () == (tick >= terminal),
                 "request completed at the wrong monotonic time");
    }
    require (sent == send_times.size (), "fixture did not observe every request attempt");
    verify_outcome (*harness.result, expect.at ("result").get<std::string> ());
    require (harness.request_callbacks == expect.at ("requestCallbacks")
               && harness.sending_hooks == expect.at ("requestSendingHooks")
               && harness.reply_hooks == expect.at ("replyReceivedHooks"),
             "logical request invoked hooks or completion more than once");
    require (harness.waiting_delays == delays && harness.timing->jitter_consumed (),
             "admission waiting callbacks or jitter do not match the rejected attempts");
    require (harness.stream_errors == 0 && harness.connector.is_connected (),
             "request-local admission result affected the connection");
    harness.timing->advance (options.request_timeout + options.max_admission_wait.value_or (0ms));
    harness.drain_writes ();
    harness.dispatch ();
    require (harness.transport->write_count () == sent && harness.request_callbacks == 1,
             "a completed logical request sent an extra attempt or completed again");
}

void verify_wait_interruption (const json &given, const json &contract, bool caller_cancelled)
{
    harness_t harness (options_for (given));
    sc::packet_t request{.name = "Login", .payload = as_bytes ("{}")};
    std::optional<zlink::stream_e2e_client::task_t<payload_t>> task;
    if (caller_cancelled) {
        task.emplace (
          zlink::stream_e2e_client::coroutine_request_call_t (harness.connector.request (request))
            .async<payload_t> ());
    } else {
        harness.submit (request);
    }
    (void) harness.read_write (0);
    harness.timing->expect_jitter (25ms);
    harness.timing->advance (10ms);
    feed_response (harness, given.at ("responses").at (0), contract);
    harness.dispatch ();
    require (harness.waiting_delays == std::vector{500ms}, "request did not enter admission wait");

    harness.timing->advance (100ms);
    if (caller_cancelled) {
        task.reset ();
    } else {
        harness.transport->end ();
    }
    harness.dispatch ();
    std::shared_ptr<fed_read_connection_t> replacement;
    if (!caller_cancelled) {
        require (harness.result && harness.result->error_code () == sc::error_code_t::disconnected,
                 "peer EOF did not complete the waiting request as Disconnected");
        auto next_transport = std::make_unique<fed_read_connection_t> ();
        require (sc::detail::change_state (harness.state, sc::connection_state_t::connected,
                                           std::nullopt, std::move (next_transport)),
                 "replacement test connection could not be installed");
        {
            std::lock_guard lock (harness.state->transport_mutex);
            replacement =
              std::static_pointer_cast<fed_read_connection_t> (harness.state->connection);
        }
        sc::detail::start_read_loop (harness.state);
    }
    harness.timing->advance (535ms);
    harness.drain_writes ();
    harness.timing->advance (3000ms);
    harness.drain_writes ();
    harness.dispatch ();

    require (harness.transport->write_count () == 1, "a stopped admission wait was retransmitted");
    {
        std::lock_guard lock (harness.state->transport_mutex);
        require (harness.state->pending_requests.empty ()
                   && harness.state->request_attempts.empty (),
                 "a stopped admission wait is still pending");
    }
    if (caller_cancelled) {
        require (harness.reply_hooks == 0 && harness.connector.is_connected (),
                 "caller cancellation ran the reply hook or disconnected the session");
    } else {
        require (harness.request_callbacks == 1 && harness.reply_hooks == 1
                   && replacement->write_count () == 0,
                 "admission request survived the connection boundary");
    }
}
} // namespace

namespace stream_connector_tests
{
int verify_login_admission ()
{
    std::string name = "fixture";
    try {
        std::ifstream input (ZLINK_LOGIN_ADMISSION_FIXTURE_PATH);
        const auto fixture = json::parse (input);
        const auto &contract = fixture.at ("contract");
        const auto &scenarios = fixture.at ("connectorScenarios");
        for (const auto &scenario : scenarios) {
            // Stage 2 requires queue tickets and position notifications.
            if (scenario.value ("stage", 1) == 2) {
                continue;
            }
            require (scenario.value ("stage", 1) == 1, "unknown admission stage");
            name = scenario.at ("name").get<std::string> ();
            run_scenario (scenario, contract);
            std::cout << "Connector admission passed " << name << '\n';
        }

        const auto &success = scenarios.at (0);
        if (sc::lz4_compression_codec ()) {
            name = "compressed-retry-then-success";
            run_scenario (success, contract, true);
            std::cout << "Connector admission passed " << name << '\n';
        }
        auto maximum = success;
        maximum["given"]["requestTimeoutMs"] = 2147485000LL;
        maximum["given"]["responses"][0]["retryAfterMs"] = "2147483647";
        maximum["given"]["responses"][0]["jitterMs"] = 50;
        maximum["given"]["responses"][1]["atElapsedMs"] = 2147483710LL;
        maximum["expect"]["sendAtElapsedMs"][1] = 2147483707LL;
        name = "maximum-valid-retry-delay";
        run_scenario (maximum, contract);
        std::cout << "Connector admission passed " << name << '\n';

        auto repeated = scenarios.at (3);
        repeated["given"]["maxAdmissionWaitMs"] = 600;
        repeated["given"]["responses"].erase (2);
        repeated["expect"]["requestSeqs"].erase (2);
        repeated["expect"]["sendAtElapsedMs"].erase (2);
        repeated["expect"]["terminalAtElapsedMs"] = 610;
        repeated["expect"]["result"] = "RequestTimeout";
        name = "repeated-rejection-keeps-first-deadline";
        run_scenario (repeated, contract);
        std::cout << "Connector admission passed " << name << '\n';

        auto stale = success;
        auto &stale_responses = stale["given"]["responses"];
        stale_responses.insert (
          stale_responses.begin () + 1,
          json{{"atElapsedMs", 250}, {"kind", "Response"}, {"requestSeq", "1"}});
        name = "stale-response-while-waiting";
        run_scenario (stale, contract);
        std::cout << "Connector admission passed " << name << '\n';

        auto other_code = scenarios.at (5);
        other_code["given"]["responses"][0]["remoteCode"] = "invalid_operation";
        other_code["given"]["responses"][0]["retryAfterMs"] = "500";
        name = "retry-hint-with-other-error-code";
        run_scenario (other_code, contract);
        std::cout << "Connector admission passed " << name << '\n';

        name = "caller-cancellation-during-wait";
        verify_wait_interruption (success.at ("given"), contract, true);
        std::cout << "Connector admission passed " << name << '\n';
        name = "peer-eof-during-wait";
        verify_wait_interruption (success.at ("given"), contract, false);
        std::cout << "Connector admission passed " << name << '\n';

        name = "invalid-max-admission-wait";
        auto invalid = options_for (success.at ("given"));
        for (const auto delay : {0ms, -1ms}) {
            invalid.max_admission_wait = delay;
            auto configured = sc::connector_factory_t::create (invalid);
            const auto result = configured.connect ();
            require (result.error_code () == sc::error_code_t::validation_failed
                       && result.error ()->message.find ("max_admission_wait") != std::string::npos,
                     "nonpositive maximum admission wait was accepted");
        }
        std::cout << "Connector admission passed " << name << '\n';
    }
    catch (const std::exception &error) {
        std::cerr << "Connector admission " << name << ": " << error.what () << '\n';
        return 1;
    }
    return 0;
}
} // namespace stream_connector_tests
