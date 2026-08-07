/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/messages.hpp"

#include <zlink/http_client.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace e2e = zlink::framework::e2e::channel_egress;
namespace http = zlink::http_client;

namespace
{

constexpr int not_found_error_kind = 0;
constexpr int not_configured_error_kind = 3;

struct client_options_t
{
    std::string scenario;
    std::string session_url;
    std::string play_url;
    std::string audit_url;
    std::string caller_url;
    std::string workflow_a_url;
    std::string workflow_b_url;
    std::string negative_url;
    std::string api_a_url;
    std::string api_b_url;
    std::string listener_url;
    std::string spot_id;
    std::string expected_workflow_rid;
    std::string expected_workflow_lifecycle;
};

client_options_t read_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown ChannelEgressRouting client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("ChannelEgressRouting client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open ChannelEgressRouting client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&section] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.scenario = value ("scenario"),
            .session_url = value ("sessionUrl"),
            .play_url = value ("playUrl"),
            .audit_url = value ("auditUrl"),
            .caller_url = value ("callerUrl"),
            .workflow_a_url = value ("workflowAUrl"),
            .workflow_b_url = value ("workflowBUrl"),
            .negative_url = value ("negativeUrl"),
            .api_a_url = value ("apiAUrl"),
            .api_b_url = value ("apiBUrl"),
            .listener_url = value ("listenerUrl"),
            .spot_id = value ("spotId"),
            .expected_workflow_rid = value ("expectedWorkflowRid"),
            .expected_workflow_lifecycle = value ("expectedWorkflowLifecycle")};
}

http::client_t make_http (const std::string &base_url)
{
    return http::client_t::create ()
      .base_url (base_url)
      .timeout (std::chrono::seconds (10))
      .build ();
}

nlohmann::json post (const std::string &base_url,
                     const std::string &path,
                     nlohmann::json body,
                     bool require_success = true)
{
    auto result = make_http (base_url)
                    .post (path)
                    .body (std::move (body))
                    .submit_raw ()
                    .result ();
    if (!result) {
        throw std::runtime_error ("POST " + path + " failed: "
                                   + (result.error () ? result.error ()->what () : "HTTP failed"));
    }
    if (require_success && result.value ().status >= 400) {
        throw std::runtime_error ("POST " + path + " returned HTTP "
                                   + std::to_string (result.value ().status));
    }
    if (result.value ().body.empty ()) {
        return nlohmann::json::object ();
    }
    return nlohmann::json::parse (result.value ().body);
}

nlohmann::json get (const std::string &base_url, const std::string &path)
{
    auto result = make_http (base_url).get (path).submit_raw ().result ();
    if (!result || result.value ().status >= 400) {
        throw std::runtime_error ("GET " + path + " failed");
    }
    return result.value ().body.empty () ? nlohmann::json::object ()
                                         : nlohmann::json::parse (result.value ().body);
}

void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

void wait_evidence (const std::string &url,
                    const std::string &contains,
                    std::chrono::seconds timeout = std::chrono::seconds (10))
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        auto result = make_http (url)
                        .get ("/evidence/wait?contains=" + contains)
                        .submit_raw ()
                        .result ();
        if (result && result.value ().status < 400) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for evidence: " + contains);
}

void wait_evidence_any (const std::vector<std::string> &urls,
                        const std::string &contains,
                        std::chrono::seconds timeout = std::chrono::seconds (10))
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        for (const auto &url : urls) {
            try {
                const auto evidence = get (url, "/evidence");
                for (const auto &entry : evidence.value ("entries", nlohmann::json::array ())) {
                    if (entry.is_string ()
                        && entry.get<std::string> ().find (contains) != std::string::npos) {
                        return;
                    }
                }
            }
            catch (...) {
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for evidence on any role: " + contains);
}

void wait_route_ready (const std::string &url,
                       const std::string &target_rid,
                       std::string_view mesh = "game")
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        auto result = make_http (url)
                        .get ("/ready?targetRid=" + target_rid + "&mesh=" + std::string (mesh))
                        .submit_raw ()
                        .result ();
        if (result && result.value ().status < 400) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for RouteMesh peer " + target_rid);
}

void wait_workflow_ready (const std::string &url)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    int attempt = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto result = post (url, "/request",
                                  { {"channel", e2e::workflow_channel},
                                    {"id", "workflow-readiness-" + std::to_string (attempt++)} },
                                  false);
        if (result.value ("succeeded", false)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for ClientServer workflow target");
}

nlohmann::json wait_listener_status (const std::string &url,
                                     std::string_view kind,
                                     std::string_view name)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            const auto result = get (url, "/listener-status?kind=" + std::string (kind)
                                           + "&name=" + std::string (name));
            if (result.value ("ready", false) && !result.value ("endpoint", "").empty ()) {
                return result;
            }
        }
        catch (...) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for listener status: " + std::string (name));
}

void wait_client_server_draining (const std::string &url)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    nlohmann::json last_status;
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            const auto result = get (url, "/client-status?channel="
                                           + std::string (e2e::workflow_channel));
            last_status = result;
            bool has_excluded = result.value ("servers", nlohmann::json::array ()).size () < 2;
            bool has_ready = false;
            for (const auto &server : result.value ("servers", nlohmann::json::array ())) {
                const auto state = server.value ("state", -1);
                has_excluded = has_excluded || state != 2
                               || server.value ("weight", 0) <= 0;
                has_ready = has_ready || state == 2;
            }
            if (has_excluded && has_ready)
                return;
        }
        catch (...) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error (
      "timed out waiting for a draining and ready ClientServer target: "
      + last_status.dump ());
}

void wait_client_server_without_target (const std::string &url)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    nlohmann::json last_status;
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            last_status = get (url, "/client-status?channel="
                                      + std::string (e2e::workflow_channel));
            if (!last_status.value ("selectable", true)
                && last_status.value ("readyServerCount", -1) == 0) {
                return;
            }
        }
        catch (...) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error (
      "timed out waiting for ClientServer target removal: " + last_status.dump ());
}

void run_ch01 (const client_options_t &options)
{
    const auto id = "ch-01-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    wait_route_ready (options.session_url, "play");
    wait_route_ready (options.play_url, "session");
    const auto forward = post (options.session_url, "/request",
                               { {"channel", e2e::play_channel}, {"id", id} });
    const auto reverse = post (options.play_url, "/request",
                               { {"channel", e2e::session_channel}, {"id", id + "-reverse"} });
    ensure (forward.value ("succeeded", false) && forward["reply"].value ("role", "") == "play",
            "CH-E2E-01 forward reply was not handled by play: " + forward.dump ());
    ensure (reverse.value ("succeeded", false) && reverse["reply"].value ("role", "") == "session",
            "CH-E2E-01 reverse reply was not handled by session: " + reverse.dump ());
    wait_evidence (options.play_url, id);
    wait_evidence (options.session_url, id + "-reverse");
}

void run_ch02 (const client_options_t &options)
{
    const auto id = "ch-02-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    wait_route_ready (options.session_url, "play");
    wait_route_ready (options.play_url, "session");
    wait_route_ready (options.play_url, "audit-audit", "audit");
    const auto result = post (options.session_url, "/request",
                              { {"channel", e2e::play_channel}, {"id", id}, {"mode", "cascade"} });
    ensure (result.value ("succeeded", false), "CH-E2E-02 outer request failed: " + result.dump ());
    ensure (result["reply"].value ("downstream", nlohmann::json::array ()).size () == 2,
            "CH-E2E-02 did not return both downstream replies: " + result.dump ());
    wait_evidence (options.audit_url, id + "-audit");
    wait_evidence_any ({options.workflow_a_url, options.workflow_b_url}, id + "-workflow");
}

void run_ch03 (const client_options_t &options)
{
    const auto id = "ch-03-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    wait_route_ready (options.caller_url, "play");
    wait_workflow_ready (options.play_url);
    wait_workflow_ready (options.caller_url);
    const auto result = post (options.caller_url, "/spot/workflow",
                              { {"spotId", options.spot_id.empty () ? id + "-spot" : options.spot_id},
                                {"id", id} });
    ensure (result.value ("succeeded", false), "CH-E2E-03 Spot request failed: " + result.dump ());
    wait_evidence (options.play_url, "spot-timer-end");
}

void run_ch04a (const client_options_t &options)
{
    constexpr int count = 800;
    wait_workflow_ready (options.caller_url);
    int succeeded = 0;
    int weighted = 0;
    for (int index = 0; index != count; ++index) {
        const auto result = post (options.caller_url, "/request",
                                  { {"channel", e2e::workflow_channel},
                                    {"id", "ch-04a-" + std::to_string (index)} });
        ensure (result.value ("succeeded", false), "CH-E2E-04A request failed");
        ++succeeded;
        if (!options.workflow_b_url.empty ()
            && result["reply"].value ("role", "") == "workflow-b") {
            ++weighted;
        }
    }
    ensure (succeeded == count, "CH-E2E-04A did not complete all requests");
    if (!options.workflow_b_url.empty ()) {
        ensure (weighted >= 520 && weighted <= 680, "CH-E2E-04A weight ratio is outside 65..85%");
    }
}

void run_ch04b (const client_options_t &options)
{
    wait_workflow_ready (options.caller_url);
    std::future<nlohmann::json> held_request;
    std::string held_id;
    bool held = false;

    for (int attempt = 0; attempt != 20 && !held; ++attempt) {
        const auto id = "ch-04b-held-" + std::to_string (attempt);
        auto candidate = std::async (
          std::launch::async,
          [&options, id] {
              return post (options.caller_url,
                           "/request",
                           { {"channel", e2e::workflow_channel},
                             {"id", id},
                             {"mode", "hold"} });
          });
        const auto deadline = std::chrono::steady_clock::now ()
                              + std::chrono::seconds (3);
        nlohmann::json completed;
        bool completed_ready = false;
        while (std::chrono::steady_clock::now () < deadline) {
            if (candidate.wait_for (std::chrono::milliseconds (50))
                == std::future_status::ready) {
                completed = candidate.get ();
                completed_ready = true;
                break;
            }
            try {
                const auto evidence = get (options.workflow_a_url, "/evidence");
                for (const auto &entry : evidence.value (
                       "entries", nlohmann::json::array ())) {
                    if (entry.is_string ()
                        && entry.get<std::string> ().find (id) != std::string::npos
                        && entry.get<std::string> ().find ("request-held")
                             != std::string::npos) {
                        held = true;
                        held_id = id;
                        held_request = std::move (candidate);
                        break;
                    }
                }
            }
            catch (...) {
            }
            if (held)
                break;
        }
        if (held)
            break;
        if (!completed_ready)
            completed = candidate.get ();
        ensure (completed.value ("succeeded", false),
                "CH-E2E-04B candidate request failed: " + completed.dump ());
        ensure (completed["reply"].value ("role", "") == "workflow-b",
                "CH-E2E-04B candidate was neither the held A request nor B: "
                  + completed.dump ());
    }

    ensure (held, "CH-E2E-04B did not observe a request held by workflow A");
    const auto shutdown = post (options.workflow_a_url, "/shutdown", {});
    ensure (shutdown.value ("status", "") == "stopping",
            "CH-E2E-04B did not start host shutdown: " + shutdown.dump ());
    wait_client_server_draining (options.caller_url);

    for (int index = 0; index != 50; ++index) {
        const auto result = post (options.caller_url,
                                  "/request",
                                  { {"channel", e2e::workflow_channel},
                                    {"id", "ch-04b-new-" + std::to_string (index)} });
        ensure (result.value ("succeeded", false),
                "CH-E2E-04B new request failed: " + result.dump ());
        ensure (result["reply"].value ("role", "") == "workflow-b",
                "CH-E2E-04B new request selected the shutting-down server: "
                  + result.dump ());
    }

    const auto completed = held_request.get ();
    ensure (completed.value ("succeeded", false),
            "CH-E2E-04B held request failed: " + completed.dump ());
    ensure (completed["reply"].value ("role", "") == "workflow-a",
            "CH-E2E-04B held request did not complete on workflow A: "
              + completed.dump ());
    (void) held_id;
}

void run_ch05 (const client_options_t &options)
{
    const auto result = post (options.negative_url, "/request",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-05"} }, false);
    ensure (!result.value ("succeeded", true), "CH-E2E-05 server-only process unexpectedly sent");
    ensure (result.value ("errorKind", -1) == not_configured_error_kind,
            "CH-E2E-05 server-only process did not return NotConfigured: " + result.dump ());
    const auto normal = post (options.caller_url, "/request",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-05-normal"} });
    ensure (normal.value ("succeeded", false), "CH-E2E-05 normal client did not send");
    std::cout << "CH-E2E-05 serverOnly=" << result.dump ()
              << " normal=" << normal.dump () << "\n";
}

void run_cpp_contract_role_001 (const client_options_t &options)
{
    wait_client_server_without_target (options.caller_url);
    const auto result = post (
      options.caller_url, "/request",
      { {"channel", e2e::workflow_channel}, {"id", "cpp-contract-role-001"} }, false);
    ensure (!result.value ("succeeded", true),
            "CPP-CONTRACT-ROLE-001 client without a target unexpectedly sent");
    ensure (result.value ("errorKind", -1) == not_found_error_kind,
            "CPP-CONTRACT-ROLE-001 client without a target did not return NotFound: "
              + result.dump ());
    std::cout << "CPP-CONTRACT-ROLE-001 noTarget=" << result.dump () << "\n";
}

void run_ch07a (const client_options_t &options)
{
    const auto result = post (options.caller_url, "/request",
                              { {"channel", "missing.channel"}, {"id", "ch-07a"} }, false);
    ensure (!result.value ("succeeded", true), "CH-E2E-07A missing channel unexpectedly succeeded");
}

void run_ch07b (const client_options_t &options)
{
    const auto url = options.api_a_url.empty () ? options.caller_url : options.api_a_url;
    if (!options.api_b_url.empty ()) {
        wait_route_ready (options.api_a_url, "api-b");
        wait_route_ready (options.api_b_url, "api-a");
    }
    for (int index = 0; index != 20; ++index) {
        const auto result = post (url, "/request",
                                  { {"channel", e2e::api_channel},
                                    {"id", "ch-07b-" + std::to_string (index)} });
        ensure (result.value ("succeeded", false), "CH-E2E-07B request failed");
    }
    if (!options.api_b_url.empty ()) {
        wait_evidence_any ({options.api_a_url, options.api_b_url}, "ch-07b-");
    }
}

void run_ch07c (const client_options_t &options)
{
    const auto result = post (options.caller_url, "/request",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-07c"} }, false);
    ensure (!result.value ("succeeded", true), "CH-E2E-07C unavailable target unexpectedly succeeded");
}

void run_ch09 (const client_options_t &options)
{
    wait_route_ready (options.session_url, "play");
    wait_route_ready (options.play_url, "session");
    const auto route = wait_listener_status (options.listener_url.empty ()
                                               ? options.play_url
                                               : options.listener_url,
                                             "route_mesh", e2e::game_mesh);
    const auto workflow = wait_listener_status (options.workflow_a_url,
                                                "client_server", e2e::workflow_channel);
    ensure (route.value ("endpoint", "").find (":0") == std::string::npos,
            "CH-E2E-09 RouteMesh advertised endpoint still contains port 0: " + route.dump ());
    ensure (workflow.value ("endpoint", "").find (":0") == std::string::npos,
            "CH-E2E-09 ClientServer advertised endpoint still contains port 0: " + workflow.dump ());
    const auto result = post (options.caller_url, "/request",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-09"} });
    ensure (result.value ("succeeded", false), "CH-E2E-09 workflow request failed: " + result.dump ());
}

void run_ch10 (const client_options_t &options)
{
    wait_workflow_ready (options.caller_url);
    const auto result = post (options.caller_url, "/send",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-10"} });
    ensure (result.value ("succeeded", false), "CH-E2E-10 send failed");
    wait_evidence (options.workflow_a_url.empty () ? options.caller_url : options.workflow_a_url,
                   "id=ch-10");
}

void run_ch04c (const client_options_t &options)
{
    wait_workflow_ready (options.caller_url);
    const auto result = post (options.caller_url, "/request",
                              { {"channel", e2e::workflow_channel}, {"id", "ch-04c"} });
    ensure (result.value ("succeeded", false), "CH-E2E-04C request failed: " + result.dump ());
    if (!options.expected_workflow_rid.empty ()) {
        ensure (result["reply"].value ("role", "") == options.expected_workflow_rid,
                "CH-E2E-04C reply was handled by the wrong lifecycle: " + result.dump ());
    }
    if (!options.expected_workflow_lifecycle.empty ()) {
        ensure (result["reply"].value ("lifecycle", "") == options.expected_workflow_lifecycle,
                "CH-E2E-04C reply did not carry the restarted lifecycle: " + result.dump ());
    }
}

void run_ch08 (const client_options_t &options)
{
    const auto id = "ch-08-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    wait_route_ready (options.session_url, "play");
    wait_route_ready (options.play_url, "session");
    wait_route_ready (options.play_url, "audit-audit", "audit");
    wait_workflow_ready (options.play_url);
    const auto result = post (options.session_url, "/request",
                              { {"channel", e2e::play_channel}, {"id", id}, {"mode", "cascade"} });
    ensure (result.value ("succeeded", false), "CH-E2E-08 request failed: " + result.dump ());
    ensure (result["reply"].value ("role", "") == "play",
            "CH-E2E-08 outer request was handled by the wrong role: " + result.dump ());
    ensure (result["reply"].value ("downstream", nlohmann::json::array ()).size () == 2,
            "CH-E2E-08 nested replies were not preserved: " + result.dump ());
    wait_evidence (options.audit_url, id + "-audit");
    wait_evidence_any ({options.workflow_a_url, options.workflow_b_url}, id + "-workflow");
}

void run_ch11 (const client_options_t &options)
{
    wait_route_ready (options.session_url, "api-a");
    wait_route_ready (options.session_url, "api-b");
    const auto result = post (options.session_url, "/request",
                              { {"channel", e2e::api_channel}, {"id", "ch-11-request"} });
    ensure (result.value ("succeeded", false), "CH-E2E-11 request failed");
    const auto send = post (options.session_url, "/send",
                            { {"channel", e2e::api_channel}, {"id", "ch-11-send"} });
    ensure (send.value ("succeeded", false), "CH-E2E-11 send failed");
    wait_evidence_any ({options.api_a_url, options.api_b_url}, "ch-11-");
}

void run_ch12 (const client_options_t &options)
{
    wait_workflow_ready (options.caller_url);
    int local = 0;
    int remote = 0;
    for (int index = 0; index != 400; ++index) {
        const auto result = post (options.caller_url, "/request",
                                  { {"channel", e2e::workflow_channel},
                                    {"id", "ch-12-" + std::to_string (index)} });
        ensure (result.value ("succeeded", false), "CH-E2E-12 request failed");
        const auto role = result["reply"].value ("role", "");
        if (role == options.expected_workflow_rid) {
            ++local;
        } else {
            ++remote;
        }
    }
    ensure (local >= 140 && remote >= 140, "CH-E2E-12 did not select local and remote servers");
}

void run_scenario (const client_options_t &options, const std::string &scenario)
{
    if (scenario == "CH-E2E-01") return run_ch01 (options);
    if (scenario == "CH-E2E-02") return run_ch02 (options);
    if (scenario == "CH-E2E-03") return run_ch03 (options);
    if (scenario == "CH-E2E-04A") return run_ch04a (options);
    if (scenario == "CH-E2E-04B") return run_ch04b (options);
    if (scenario == "CH-E2E-04C") return run_ch04c (options);
    if (scenario == "CH-E2E-05") return run_ch05 (options);
    if (scenario == "CPP-CONTRACT-ROLE-001") return run_cpp_contract_role_001 (options);
    if (scenario == "CH-E2E-07A") return run_ch07a (options);
    if (scenario == "CH-E2E-07B") return run_ch07b (options);
    if (scenario == "CH-E2E-07C") return run_ch07c (options);
    if (scenario == "CH-E2E-09") return run_ch09 (options);
    if (scenario == "CH-E2E-10") return run_ch10 (options);
    if (scenario == "CH-E2E-11") return run_ch11 (options);
    if (scenario == "CH-E2E-12") return run_ch12 (options);
    if (scenario == "CH-E2E-08") return run_ch08 (options);
    if (scenario == "CH-E2E-04B" || scenario == "CH-E2E-06"
        || scenario == "CH-E2E-08") {
        throw std::runtime_error (scenario + " requires a dedicated process fixture");
    }
    throw std::runtime_error ("unknown ChannelEgressRouting scenario: " + scenario);
}

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = read_options (argc, argv);
        std::vector<std::string> scenarios;
        if (options.scenario.empty () || options.scenario == "all") {
            scenarios = {"CH-E2E-01", "CH-E2E-02", "CH-E2E-03", "CH-E2E-04A",
                         "CH-E2E-05", "CPP-CONTRACT-ROLE-001", "CH-E2E-07A",
                         "CH-E2E-07B", "CH-E2E-07C",
                         "CH-E2E-10", "CH-E2E-11", "CH-E2E-12"};
        } else {
            scenarios = {options.scenario};
        }
        for (const auto &scenario : scenarios) {
            run_scenario (options, scenario);
            std::cout << scenario << " PASS\n";
        }
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "channel-egress scenario failed: " << error.what () << "\n";
        return 1;
    }
}
