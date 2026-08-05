/* SPDX-License-Identifier: MPL-2.0 */

/* Config 11 trigger client. Scenarios:
 *   flow  — OBS-A1: one STREAM request whose connector-generated flow id
 *           threads connector -> session inbound -> room-spot dispatch.
 *   error — OBS-A2: a packet without a handler so the dispatch error line
 *           carries the same flow id as the healthy lines.
 *   hold-session — OBS-C4: keeps the session open until the server force
 *           stops; prints the public closeReason the connector observed.
 * The trigger only uses the public connector surface. */

#include "../Shared/observability_contracts.hpp"
#include "../Client/Support/client_runner.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace obs = zlink::framework::e2e::observability_ops;

namespace
{

struct trigger_options_t
{
    std::string scenario;
    std::string stream_endpoint;
    std::string spot_id;
    std::optional<zlink::framework::e2e::observability_ops::client::verification_input_t>
      verification;
};

trigger_options_t read_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown ObservabilityOps trigger option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("ObservabilityOps trigger requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open ObservabilityOps trigger config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    if (section.contains ("verification")) {
        const auto &verification = section.at ("verification");
        return {.verification =
                  zlink::framework::e2e::observability_ops::client::verification_input_t{
                    .scenario_id = verification.at ("scenarioId").get<std::string> (),
                    .files = verification.at ("files").get<std::map<std::string, std::string>> ()}};
    }
    return {.scenario = section.at ("scenario").get<std::string> (),
            .stream_endpoint = section.at ("streamEndpoint").get<std::string> (),
            .spot_id = section.at ("spotId").get<std::string> ()};
}

void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

} // namespace

int zlink::framework::e2e::observability_ops::client::run (int argc, char **argv)
{
    try {
        const auto configured = read_options (argc, argv);
        if (configured.verification) {
            return run_scenario_verification (*configured.verification);
        }
        const auto &scenario = configured.scenario;
        const auto &stream_endpoint = configured.stream_endpoint;
        const auto &spot_id = configured.spot_id;
        ensure (!stream_endpoint.empty (), "streamEndpoint is required");

        zlink::stream_connector::connector_options_t options;
        options.endpoint = stream_endpoint;
        options.connect_timeout = std::chrono::milliseconds (3000);
        options.request_timeout = std::chrono::milliseconds (10000);
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto client = zlink::stream_connector::connector_factory_t::create (options);
        auto connected = client.connect ();
        ensure (static_cast<bool> (connected), "ObservabilityOps stream connect failed");

        if (scenario == "flow" || scenario == "fanout") {
            auto reply =
              client
                .request (obs::obs_action_req_t{.spot_id = spot_id,
                                                .marker = scenario == "flow" ? "obs-a1" : "obs-a4",
                                                .value = 7})
                .packet_name (obs::obs_action_req_t::packet_name)
                .timeout (std::chrono::milliseconds (10000))
                .submit<obs::obs_action_res_t> ();
            ensure (static_cast<bool> (reply), "OBS action request failed");
            if (scenario == "flow") {
                ensure (reply.value ().value == 7, "OBS-A1 unexpected room value");
            } else {
                // the room accumulates across scenarios; only delivery matters
                ensure (reply.value ().value >= 7, "OBS-A4 room value missing");
            }
            std::cout << "scenario " << scenario << " trigger passed" << std::endl;
            return 0;
        }
        if (scenario == "hold-session") {
            /* OBS-C4: keep the session open through the force stop; the
             * server sends session-closing(server_drain) and the connector
             * exposes the reason through the public closeReason. */
            std::mutex gate;
            std::condition_variable changed;
            std::optional<zlink::stream_connector::close_reason_t> observed;
            client.on_connection_state_changed (
              [&] (const zlink::stream_connector::connection_state_changed_t &event) {
                  if (event.current == zlink::stream_connector::connection_state_t::disconnected
                      || event.current == zlink::stream_connector::connection_state_t::closed) {
                      const std::lock_guard<std::mutex> lock (gate);
                      if (!observed && event.close_reason) {
                          observed = event.close_reason;
                      }
                      changed.notify_all ();
                  }
              });
            // one action proves the session is live before the drain begins
            auto warm = client
                          .request (obs::obs_action_req_t{
                            .spot_id = spot_id, .marker = "obs-c4", .value = 1})
                          .packet_name (obs::obs_action_req_t::packet_name)
                          .timeout (std::chrono::milliseconds (10000))
                          .submit<obs::obs_action_res_t> ();
            ensure (static_cast<bool> (warm),
                    std::string ("OBS-C4 warm-up action failed: ")
                      + (warm.error () ? warm.error ()->message : "unknown error"));
            std::cout << "hold-session ready" << std::endl;
            /* Keep dispatching: the connector pump answers server liveness
             * pings and surfaces the session-closing control; a passive wait
             * would starve both and the server would see a pong timeout. */
            const auto wait_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (30);
            bool arrived = false;
            while (std::chrono::steady_clock::now () < wait_deadline) {
                (void) client.dispatch ();
                {
                    const std::lock_guard<std::mutex> lock (gate);
                    if (observed) {
                        arrived = true;
                        break;
                    }
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
            ensure (arrived, "OBS-C4 no close reason observed within 30s");
            const auto reason = *observed;
            const char *name =
              reason == zlink::stream_connector::close_reason_t::server_drain   ? "server_drain"
              : reason == zlink::stream_connector::close_reason_t::client_close ? "client_close"
              : reason == zlink::stream_connector::close_reason_t::idle_timeout ? "idle_timeout"
              : reason == zlink::stream_connector::close_reason_t::heartbeat_timeout
                ? "heartbeat_timeout"
              : reason == zlink::stream_connector::close_reason_t::protocol_error
                ? "protocol_error"
                : "transport_error";
            std::cout << "hold-session closeReason=" << name << std::endl;
            return 0;
        }
        if (scenario == "error") {
            auto failed = client.request (obs::obs_unknown_req_t{.marker = "obs-a2"})
                            .packet_name (obs::obs_unknown_req_t::packet_name)
                            .timeout (std::chrono::milliseconds (3000))
                            .submit<obs::obs_action_res_t> ();
            ensure (!failed, "OBS-A2 expected the unknown packet to fail");
            std::cout << "scenario OBS-A2 trigger passed" << std::endl;
            return 0;
        }
        throw std::runtime_error ("unknown ObservabilityOps trigger scenario: " + scenario);
    }
    catch (const std::exception &error) {
        std::cerr << "observability-ops trigger failed: " << error.what () << std::endl;
        return 1;
    }
}
