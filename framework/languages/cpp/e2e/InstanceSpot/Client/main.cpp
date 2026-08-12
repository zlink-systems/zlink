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

namespace e2e = zlink::framework::e2e::instance_spot;
namespace http = zlink::http_client;

namespace
{

struct client_options_t
{
    std::string caller_url;
    std::string owner_url;
    std::string owner_rid;
};

client_options_t read_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            break;
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("InstanceSpot client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open InstanceSpot client config: " + path);
    }
    const auto config = nlohmann::json::parse (input).at ("e2e");
    return {.caller_url = config.at ("callerUrl").get<std::string> (),
            .owner_url = config.at ("ownerUrl").get<std::string> (),
            .owner_rid = config.at ("ownerRid").get<std::string> ()};
}

http::client_t make_http (const std::string &base_url)
{
    return http::client_t::create ().base_url (base_url).timeout (std::chrono::seconds (10)).build ();
}

nlohmann::json post (const std::string &base_url,
                     const std::string &path,
                     nlohmann::json body)
{
    auto result = make_http (base_url).post (path).body (std::move (body)).submit_raw ().result ();
    if (!result || result.value ().status >= 400) {
        const auto error = result ? result.value ().body
                                  : (result.error () ? result.error ()->what () : "HTTP failed");
        throw std::runtime_error ("POST " + path + " failed: " + error);
    }
    return nlohmann::json::parse (result.value ().body);
}

http::raw_http_response_t post_raw (const std::string &base_url,
                                    const std::string &path,
                                    nlohmann::json body)
{
    auto result = make_http (base_url)
                    .post (path)
                    .body (std::move (body))
                    .submit_raw ()
                    .result ();
    if (!result) {
        throw std::runtime_error (
          result.error () ? result.error ()->what () : "HTTP failed");
    }
    return std::move (result.value ());
}

e2e::operation_evidence_t evidence (const client_options_t &options,
                                    const std::string &operation_id)
{
    auto result = make_http (options.owner_url)
                    .get ("/evidence?operationId=" + operation_id)
                    .submit_raw ()
                    .result ();
    if (!result || result.value ().status >= 400) {
        throw std::runtime_error ("GET /evidence failed");
    }
    return nlohmann::json::parse (result.value ().body).get<e2e::operation_evidence_t> ();
}

void wait_ready (const client_options_t &options)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        auto result = make_http (options.caller_url)
                        .get ("/ready?targetRid=" + options.owner_rid)
                        .submit_raw ()
                        .result ();
        if (result && result.value ().status < 400) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("InstanceSpot route did not become ready");
}

void wait_evidence (const client_options_t &options, const std::string &operation_id)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        const auto current = evidence (options, operation_id);
        if (current.completed == 1) {
            if (current.entered != 1) {
                throw std::runtime_error ("InstanceSpot handler entered more than once");
            }
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("InstanceSpot evidence did not converge: " + operation_id);
}

void run_request (const client_options_t &options,
                  std::string spot_id,
                  std::string operation_id,
                  std::string action)
{
    const auto expected_spot_id = spot_id;
    const auto reply = post (options.caller_url, "/instance/request",
                             e2e::probe_req_t{std::move (spot_id), operation_id,
                                                  std::move (action)});
    if (reply.value ("spotId", "") != expected_spot_id
        || reply.value ("operationId", "") != operation_id
        || !reply.value ("instanceSpot", false)) {
        throw std::runtime_error ("InstanceSpot request reply does not preserve its contract");
    }
    wait_evidence (options, operation_id);
}

void run_send (const client_options_t &options,
               std::string spot_id,
               std::string operation_id,
               std::string action)
{
    const auto request = e2e::probe_msg_t{spot_id, operation_id, std::move (action)};
    const auto reply = post (options.caller_url, "/instance/send", request);
    if (reply.value ("status", "") != "accepted"
        || reply.value ("spotId", "") != spot_id
        || reply.value ("operationId", "") != operation_id) {
        throw std::runtime_error ("InstanceSpot send did not return accepted");
    }
    wait_evidence (options, operation_id);
}

void run_scenario (const client_options_t &options, const std::string &scenario)
{
    if (scenario == "IS-E2E-05") {
        const auto operation_id = scenario + "-after-crash";
        const auto response = post_raw (
          options.caller_url, "/instance/request",
          e2e::probe_req_t{scenario + "-spot", operation_id, scenario});
        const auto body = nlohmann::json::parse (response.body);
        if (response.status < 400
            || body.value ("errorKind", -1)
                 != static_cast<int> (
                   zlink::framework::framework_error_kind_t::unavailable)) {
            throw std::runtime_error (
              "IS-E2E-05 request did not terminate as Unavailable");
        }
        std::cout << "IS-E2E-05 terminal=Unavailable operation="
                  << operation_id << '\n';
        return;
    }
    wait_ready (options);
    if (scenario == "IS-E2E-02") {
        run_send (options, scenario + "-spot", scenario + "-send", scenario);
        return;
    }
    if (scenario == "IS-E2E-03") {
        std::vector<std::future<void>> calls;
        for (int index = 0; index != 4; ++index) {
            calls.push_back (std::async (
              std::launch::async,
              [&options, index] {
                  const auto id = std::string ("IS-E2E-03-") + std::to_string (index);
                  run_request (options, "IS-E2E-03-shared", id, "IS-E2E-03");
              }));
        }
        for (auto &call : calls) {
            call.get ();
        }
        return;
    }
    if (scenario == "IS-E2E-17") {
        std::vector<std::future<void>> calls;
        for (int index = 0; index != 3; ++index) {
            calls.push_back (std::async (
              std::launch::async,
              [&options, index] {
                  const auto id = std::string ("IS-E2E-17-") + std::to_string (index);
                  run_request (options, id + "-spot", id, "IS-E2E-17");
              }));
        }
        for (auto &call : calls) {
            call.get ();
        }
        return;
    }
    throw std::runtime_error (
      "InstanceSpot scenario is registered but not implemented: " + scenario);
}

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = read_options (argc, argv);
        const std::vector<std::string> known_scenarios{
          "IS-E2E-01", "IS-E2E-02", "IS-E2E-03", "IS-E2E-04", "IS-E2E-05",
          "IS-E2E-06", "IS-E2E-07", "IS-E2E-08", "IS-E2E-09", "IS-E2E-10",
          "IS-E2E-11", "IS-E2E-12", "IS-E2E-13", "IS-E2E-14", "IS-E2E-15",
          "IS-E2E-16", "IS-E2E-17", "IS-E2E-18", "IS-E2E-19", "IS-E2E-20",
          "IS-E2E-21", "IS-E2E-22", "IS-E2E-23", "IS-E2E-24", "IS-E2E-25",
          "IS-E2E-26", "IS-E2E-27", "IS-E2E-28", "IS-E2E-29", "IS-E2E-30",
          "IS-E2E-31", "IS-E2E-32", "IS-E2E-33", "IS-E2E-34", "IS-E2E-35",
          "IS-E2E-36"};
        std::vector<std::string> selected;
        for (int index = 2; index < argc; ++index) {
            if (std::string (argv[index]) == "all") {
                selected = known_scenarios;
                break;
            }
            selected.emplace_back (argv[index]);
        }
        if (selected.empty ()) {
            selected = known_scenarios;
        }
        for (const auto &scenario : selected) {
            if (std::find (known_scenarios.begin (), known_scenarios.end (), scenario)
                == known_scenarios.end ()) {
                throw std::runtime_error ("unknown InstanceSpot scenario: " + scenario);
            }
            run_scenario (options, scenario);
            std::cout << scenario << " PASS\n";
        }
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "instance-spot scenario failed: " << error.what () << "\n";
        return 1;
    }
}
