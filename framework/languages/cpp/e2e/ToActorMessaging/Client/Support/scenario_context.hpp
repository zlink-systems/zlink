/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/messages.hpp"

#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace e2e = zlink::e2e::to_actor_messaging;
namespace sc = zlink::stream_connector;

namespace
{

struct client_configuration_t
{
    std::string actor_http;
    std::string actor_b_http;
    std::string caller_http;
    std::string route_control_http;
    std::string session_a_http;
    std::string session_a_stream;
    std::string session_b_http;
    std::string session_b_stream;
    std::string scenario = "all";
};

client_configuration_t parse_client_configuration (int argc, char **argv)
{
    client_configuration_t configuration;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto assign = [&] (const std::string &prefix, std::string &target) {
            if (argument.rfind (prefix, 0) != 0) {
                return false;
            }
            target = argument.substr (prefix.size ());
            return true;
        };
        if (!assign ("--actor-http=", configuration.actor_http)
            && !assign ("--actor-b-http=", configuration.actor_b_http)
            && !assign ("--caller-http=", configuration.caller_http)
            && !assign ("--route-control-http=", configuration.route_control_http)
            && !assign ("--session-a-http=", configuration.session_a_http)
            && !assign ("--session-a-stream=", configuration.session_a_stream)
            && !assign ("--session-b-http=", configuration.session_b_http)
            && !assign ("--session-b-stream=", configuration.session_b_stream)
            && !assign ("--scenario=", configuration.scenario)) {
            throw std::runtime_error ("unknown ToActorMessaging client option: " + argument);
        }
    }
    if (configuration.actor_http.empty () || configuration.actor_b_http.empty ()) {
        throw std::runtime_error ("both actor owner HTTP endpoints are required");
    }
    if (configuration.caller_http.empty ()) {
        throw std::runtime_error ("--caller-http is required");
    }
    if (configuration.route_control_http.empty ()) {
        throw std::runtime_error ("--route-control-http is required");
    }
    if (configuration.session_a_http.empty () || configuration.session_a_stream.empty ()
        || configuration.session_b_http.empty () || configuration.session_b_stream.empty ()) {
        throw std::runtime_error ("both session gateway HTTP and STREAM endpoints are required");
    }
    if (configuration.scenario.empty ()) {
        throw std::runtime_error ("--scenario must not be empty");
    }
    return configuration;
}

void require (bool condition, const std::string &message);

class bound_actor_session_t
{
  public:
    bound_actor_session_t (const std::string &endpoint,
                           const std::string &scenario,
                           const std::string &actor_id)
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (10);
        options.heartbeat.enabled = false;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        _connector.emplace (sc::connector_factory_t::create (options));
        require (static_cast<bool> (_connector->connect ()), scenario + " stream connect failed");
        auto bound = _connector
                       ->request (e2e::bind_actor_session_req_t{scenario, actor_id})
                       .packet_name (e2e::bind_actor_session_req_t::packet_name)
                       .submit<e2e::bind_actor_session_res_t> ();
        require (static_cast<bool> (bound), scenario + " stream bind failed");
        require (bound.value ().actor_id == actor_id, scenario + " stream bind actor mismatch");
    }

    ~bound_actor_session_t ()
    {
        close ();
    }

    void close ()
    {
        if (_connector) {
            _connector->close ();
            _connector.reset ();
        }
    }

    std::future<e2e::actor_push_notify_t> expect_push (const std::string &scenario)
    {
        auto promise = std::make_shared<std::promise<e2e::actor_push_notify_t>> ();
        auto future = promise->get_future ();
        _connector->wait_for<e2e::actor_push_notify_t> (e2e::actor_push_notify_t::packet_name)
          .where ([scenario] (const e2e::actor_push_notify_t &notify) {
              return notify.scenario == scenario;
          })
          .timeout (std::chrono::seconds (10))
          .submit ([promise] (sc::result_t<e2e::actor_push_notify_t> result) {
              if (result) {
                  promise->set_value (std::move (result.value ()));
              } else {
                  promise->set_exception (std::make_exception_ptr (
                    std::runtime_error ("bound actor push wait failed")));
              }
          });
        return future;
    }

  private:
    std::optional<sc::connector_t> _connector;
};

void require (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

zlink::http_client::client_t make_http (const std::string &base_url)
{
    return zlink::http_client::client_t::create ()
      .base_url (base_url)
      .timeout (std::chrono::seconds (30))
      .build ();
}

e2e::actor_call_response_t call (zlink::http_client::client_t &caller,
                                 const std::string &endpoint,
                                 const std::string &scenario,
                                 const std::string &actor_id,
                                 const std::string &value);

void push_actor (zlink::http_client::client_t &actor,
                 const std::string &scenario,
                 const std::string &actor_id,
                 const std::string &value)
{
    const auto response = call (actor, "/push", scenario, actor_id, value);
    require (response.error_kind.empty () && response.result == "pushed",
             scenario + " actor push failed: " + response.error_kind);
}

std::vector<e2e::actor_evidence_t> session_evidence (zlink::http_client::client_t &session)
{
    return session.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
}

void wait_session_evidence (zlink::http_client::client_t &session,
                            const std::string &scenario,
                            const std::string &actor_id,
                            const std::string &kind)
{
    const auto response = call (session, "/evidence/wait", scenario, actor_id, kind);
    require (response.result == "observed", scenario + " session " + kind + " not observed");
}

std::size_t count_session_evidence (const std::vector<e2e::actor_evidence_t> &evidence,
                                    const std::string &actor_id,
                                    const std::string &kind)
{
    std::size_t count = 0;
    for (const auto &entry : evidence) {
        if (entry.actor_id == actor_id && entry.kind == kind) {
            ++count;
        }
    }
    return count;
}

void require_session_evidence (const std::vector<e2e::actor_evidence_t> &evidence,
                               const std::string &actor_id,
                               const std::string &kind,
                               const std::string &gateway_rid)
{
    for (const auto &entry : evidence) {
        if (entry.actor_id == actor_id && entry.kind == kind && entry.value == gateway_rid) {
            return;
        }
    }
    throw std::runtime_error (actor_id + " has no " + kind + " evidence for " + gateway_rid);
}

e2e::actor_call_response_t call (zlink::http_client::client_t &caller,
                                 const std::string &endpoint,
                                 const std::string &scenario,
                                 const std::string &actor_id,
                                 const std::string &value)
{
    return caller.post (endpoint)
      .body (e2e::actor_call_request_t{scenario, actor_id, value})
      .submit<e2e::actor_call_response_t> ().result ().value ().body;
}

void ensure_actor (zlink::http_client::client_t &actor,
                   const std::string &scenario,
                   const std::string &actor_id)
{
    const auto response = actor.post ("/ensure")
                            .body (e2e::actor_call_request_t{scenario, actor_id, "ensure"})
                            .submit<e2e::actor_call_response_t> ().result ().value ().body;
    require (response.error_kind.empty (), scenario + " ensure failed: " + response.error_kind);
    require (response.result == "ensured", scenario + " ensure returned " + response.result);
}

void wait_until_ready (zlink::http_client::client_t &caller,
                       const std::string &scenario,
                       const std::string &actor_id)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (20);
    e2e::actor_call_response_t response;
    while (std::chrono::steady_clock::now () < deadline) {
        response = call (caller, "/request", scenario, actor_id, "ready");
        if (response.error_kind.empty () && response.result == "reply:ready") {
            return;
        }
        if (response.error_kind != "request_failed"
            && response.error_kind != "actor_route_not_found") {
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error (scenario + " readiness failed: " + response.error_kind + " "
                              + response.result);
}

void ensure_ready (zlink::http_client::client_t &actor,
                   zlink::http_client::client_t &caller,
                   const std::string &scenario,
                   const std::string &actor_id)
{
    ensure_actor (actor, scenario, actor_id);
    wait_until_ready (caller, scenario + "-ready", actor_id);
}

void assert_call (zlink::http_client::client_t &caller,
                  const std::string &scenario,
                  const std::string &actor_id,
                  const std::string &value,
                  const std::string &expected,
                  bool send)
{
    const auto response = call (caller, send ? "/send" : "/request", scenario, actor_id, value);
    require (response.error_kind.empty (), scenario + " unexpected error: " + response.error_kind);
    require (response.result == expected,
             scenario + " expected " + expected + " got " + response.result);
}

void assert_failure (zlink::http_client::client_t &caller,
                     const std::string &scenario,
                     const std::string &actor_id,
                     const std::string &expected_kind,
                     bool send)
{
    const auto response = call (caller, send ? "/send" : "/request", scenario, actor_id, "missing");
    require (response.error_kind == expected_kind, scenario + " expected " + expected_kind + " got "
                                                     + response.error_kind
                                                     + " result=" + response.result);
}

void capture_ref (zlink::http_client::client_t &caller,
                  const std::string &scenario,
                  const std::string &actor_id)
{
    const auto response = call (caller, "/capture-ref", scenario, actor_id, "capture");
    require (response.error_kind.empty () && response.result == "captured",
             scenario + " actor ref capture failed: " + response.error_kind);
}

void assert_captured_call (zlink::http_client::client_t &caller,
                           const std::string &scenario,
                           const std::string &actor_id,
                           const std::string &value,
                           const std::string &expected)
{
    const auto response = call (caller, "/request-captured", scenario, actor_id, value);
    require (response.error_kind.empty () && response.result == expected,
             scenario + " captured request failed: " + response.error_kind
               + " result=" + response.result + " expected=" + expected);
}

void assert_captured_failure (zlink::http_client::client_t &caller,
                              const std::string &scenario,
                              const std::string &actor_id,
                              const std::string &expected_kind)
{
    const auto response = call (caller, "/request-captured", scenario, actor_id, "failure");
    require (response.error_kind == expected_kind,
             scenario + " expected " + expected_kind + " got " + response.error_kind
               + " result=" + response.result);
}

void control_route (zlink::http_client::client_t &control, const std::string &operation)
{
    const auto response = control.post ("/route/" + operation)
                            .body (nlohmann::json::object ())
                            .submit<nlohmann::json> ().result ().value ().body;
    require (response.value ("status", "") == operation,
             "route control " + operation + " failed");
}

void require_evidence (const std::vector<e2e::actor_evidence_t> &evidence,
                       const std::string &scenario,
                       const std::string &kind)
{
    for (const auto &entry : evidence) {
        if (entry.scenario == scenario && entry.kind == kind) {
            return;
        }
    }
    throw std::runtime_error (scenario + " " + kind + " evidence missing");
}

void require_no_evidence (const std::vector<e2e::actor_evidence_t> &evidence,
                          const std::string &scenario)
{
    for (const auto &entry : evidence) {
        require (entry.scenario != scenario,
                 scenario + " unexpectedly reached actor handler kind=" + entry.kind);
    }
}

void require_location (zlink::http_client::client_t &caller,
                       const std::string &scenario,
                       const std::string &actor_id,
                       const std::string &expected)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    e2e::actor_call_response_t response;
    while (std::chrono::steady_clock::now () < deadline) {
        response = call (caller, "/location", scenario, actor_id, "observe");
        if (response.error_kind.empty () && response.result == expected) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error (scenario + " expected location=" + expected
                              + " result=" + response.result + " error=" + response.error_kind);
}

std::vector<std::string> split_selector (std::string selector)
{
    if (selector.empty ()) {
        selector = "all";
    }
    std::vector<std::string> scenarios;
    std::stringstream stream (selector);
    std::string item;
    while (std::getline (stream, item, ',')) {
        if (!item.empty ()) {
            scenarios.push_back (item);
        }
    }
    if (scenarios.empty ()) {
        scenarios.push_back ("all");
    }
    return scenarios;
}

bool should_run (const std::vector<std::string> &selected,
                 std::initializer_list<const char *> names)
{
    for (const auto &scenario : selected) {
        if (scenario == "all") {
            return true;
        }
        for (const auto *name : names) {
            if (scenario == name) {
                return true;
            }
        }
    }
    return false;
}

void validate_selector (const std::vector<std::string> &selected)
{
    for (const auto &scenario : selected) {
        if (should_run ({scenario},
                        {"TA-A1", "ta-a1", "TA-A2", "ta-a2", "TA-A3", "ta-a3", "TA-A4", "ta-a4",
                         "TA-B1", "ta-b1", "TA-B2", "ta-b2", "TA-B3", "ta-b3"})) {
            continue;
        }
        throw std::runtime_error ("Unsupported ToActorMessaging scenario: " + scenario);
    }
}


} // namespace
