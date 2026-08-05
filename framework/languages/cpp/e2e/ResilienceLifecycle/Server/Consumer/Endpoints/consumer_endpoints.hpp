/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/resilience_lifecycle_contracts.hpp"
#include "../../Shared/location_store.hpp"
#include "../../Shared/public_error_type.hpp"
#include "../Configuration/consumer_options.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::consumer
{

using resilience_lifecycle::api_channel;
using resilience_lifecycle::operation_status_t;
using resilience_lifecycle::profile_msg_t;
using resilience_lifecycle::profile_req_t;
using resilience_lifecycle::profile_res_t;
using resilience_lifecycle::request_failure_res_t;

struct topology_entry_result_t
{
    std::string name;
    std::string kind;
    std::string role;
    std::string routing_id;
    std::string endpoint;
    std::string state;
    std::uint32_t weight = 100;
};

inline void to_json (nlohmann::json &json, const topology_entry_result_t &value)
{
    json = nlohmann::json{{"name", value.name},
                          {"kind", value.kind},
                          {"role", value.role},
                          {"routing_id", value.routing_id},
                          {"endpoint", value.endpoint},
                          {"state", value.state},
                          {"weight", value.weight}};
}

inline const char *client_server_state_name (
  zlink::framework::client_server_server_state_t state)
{
    switch (state) {
        case zlink::framework::client_server_server_state_t::ready:
            return "Ready";
        case zlink::framework::client_server_server_state_t::draining:
            return "Draining";
        default:
            return "NotReady";
    }
}

inline std::vector<topology_entry_result_t> peer_topology (
  zlink::framework::client_server_runtime_t &runtime,
  const std::optional<zlink::routing_id_t> &routing_id = std::nullopt,
  zlink::framework::location_role_t role = zlink::framework::location_role_t::router)
{
    const auto snapshot = runtime.snapshot (api_channel);
    std::vector<topology_entry_result_t> entries;
    entries.reserve (snapshot.servers.size ());
    for (const auto &server : snapshot.servers) {
        if (routing_id && server.server_rid != *routing_id)
            continue;
        entries.push_back (topology_entry_result_t{.name = snapshot.channel_name,
                                                   .kind = "channel",
                                                   .role = role == zlink::framework::location_role_t::dealer
                                                             ? "dealer"
                                                             : "router",
                                                   .routing_id = server.server_rid.to_string (),
                                                   .endpoint = {},
                                                   .state = client_server_state_name (server.state),
                                                   .weight = static_cast<std::uint32_t> (
                                                     std::max (0, server.weight))});
    }
    return entries;
}

inline zlink::framework::location_role_t parse_topology_role (const std::string &role)
{
    if (role == "dealer") {
        return zlink::framework::location_role_t::dealer;
    }
    if (role == "router" || role.empty ()) {
        return zlink::framework::location_role_t::router;
    }
    throw std::runtime_error ("unsupported topology role: " + role);
}

class topology_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::client_server_runtime_t>;

    explicit topology_handler_t (zlink::framework::client_server_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body.empty () ? "{}" : request.body);
        const auto role = parse_topology_role (body.value ("role", std::string{"router"}));
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (peer_topology (_runtime, std::nullopt, role)).dump ();
        return response;
    }

  private:
    zlink::framework::client_server_runtime_t &_runtime;
};

class topology_wait_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::client_server_runtime_t>;

    explicit topology_wait_handler_t (zlink::framework::client_server_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body.empty () ? "{}" : request.body);
        const auto routing_id =
          body.value ("routingId", body.value ("routing_id", std::string{}));
        auto state = body.value ("state", std::string{"Ready"});
        if (state == "active") {
            state = "Ready";
        }
        const auto expected_count =
          body.value ("expectedCount", body.value ("expected_count", 1));
        const auto role = parse_topology_role (body.value ("role", std::string{"router"}));
        const auto timeout_ms =
          std::clamp (body.value ("timeoutMilliseconds",
                                  body.value ("timeout_milliseconds", 30000)),
                      1,
                      30000);
        const auto rid = routing_id.empty ()
                           ? std::optional<zlink::routing_id_t>{}
                           : zlink::routing_id_t::from (routing_id);
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
        while (std::chrono::steady_clock::now () < deadline) {
            auto entries = peer_topology (_runtime, rid, role);
            const auto count =
              std::count_if (entries.begin (), entries.end (), [&] (const auto &entry) {
                  return (routing_id.empty () || entry.routing_id == routing_id)
                         && entry.state == state;
              });
            const auto satisfied =
              expected_count == 0 ? count == 0 : count >= expected_count;
            if (satisfied) {
                zlink::framework::http_response_t response;
                response.body = nlohmann::json (entries).dump ();
                return response;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (250));
        }

        zlink::framework::http_response_t response;
        response.status = 504;
        response.body = nlohmann::json{{"error", "topology did not reach expected state"},
                                       {"routing_id", routing_id},
                                       {"state", state},
                                       {"expected_count", expected_count}}
                          .dump ();
        return response;
    }

  private:
    zlink::framework::client_server_runtime_t &_runtime;
};

inline profile_res_t request_profile_once (zlink::framework::channel_client_t &channels,
                                           const profile_req_t &request,
                                           std::chrono::milliseconds timeout)
{
    auto call = channels.request (api_channel, request)
                  .timeout (timeout)
                  .submit<profile_res_t> ();
    const auto &reply = call.result ();
    if (reply) {
        return reply.value ();
    }
    throw std::runtime_error (reply.error () ? reply.error ()->what ()
                                             : "profile request failed");
}

class profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit profile_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        return request_profile_once (_channels, request, std::chrono::milliseconds (3000));
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class manual_profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit manual_profile_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request ("resilience.lifecycle.api.manual", request)
                      .timeout (std::chrono::milliseconds (3000))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (reply) {
            return reply.value ();
        }
        throw std::runtime_error (reply.error () ? reply.error ()->what ()
                                                 : "manual profile request failed");
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class manual_b_profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit manual_b_profile_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request ("resilience.lifecycle.api.manual.b", request)
                      .timeout (std::chrono::milliseconds (3000))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (reply) {
            return reply.value ();
        }
        throw std::runtime_error (reply.error () ? reply.error ()->what ()
                                                 : "manual provider B profile request failed");
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class slow_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = request_failure_res_t;

    explicit slow_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    request_failure_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request (api_channel, request)
                      .timeout (std::chrono::milliseconds (100))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (reply) {
            return {.failed = false, .error_type = ""};
        }
        return {.failed = true,
                .error_type = resilience_lifecycle::public_error_type (reply)};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class missing_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = request_failure_res_t;

    explicit missing_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    request_failure_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request (api_channel, missing_profile_req_t{request})
                      .timeout (std::chrono::milliseconds (3000))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (reply) {
            return {.failed = false, .error_type = ""};
        }
        return {.failed = true,
                .error_type = resilience_lifecycle::public_error_type (reply)};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class profile_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_msg_t;
    using reply_type = operation_status_t;

    explicit profile_command_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    operation_status_t handle (const profile_msg_t &command)
    {
        _channels.send (api_channel, command).submit ();
        return {.status = "sent"};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class missing_profile_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_msg_t;
    using reply_type = operation_status_t;

    explicit missing_profile_command_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    operation_status_t handle (const profile_msg_t &command)
    {
        _channels.send (api_channel, missing_profile_msg_t{command}).submit ();
        return {.status = "sent"};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class transient_profile_request_service_t final : public zlink::framework::hosted_service_t
{
  public:
    struct result_state_t
    {
        std::optional<profile_res_t> reply;
        std::optional<std::string> error;

        profile_res_t take_reply () const
        {
            if (error) {
                throw std::runtime_error (*error);
            }
            if (!reply) {
                throw std::runtime_error ("transient profile request produced no reply");
            }
            return *reply;
        }
    };

    transient_profile_request_service_t (zlink::framework::app_t &app,
                                         profile_req_t request,
                                         std::shared_ptr<result_state_t> result) :
        _app (app), _request (std::move (request)), _result (std::move (result))
    {
    }

    void start (zlink::framework::service_provider_t &services) override
    {
        try {
            auto &channels = services.get_required<zlink::framework::channel_client_t> ();
            _result->reply =
              request_profile_once (channels, _request, std::chrono::milliseconds (30000));
        }
        catch (const std::exception &error) {
            _result->error = error.what ();
        }
        _app.stop ();
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t &_app;
    profile_req_t _request;
    std::shared_ptr<result_state_t> _result;
};

class transient_route_request_service_t final :
    public zlink::framework::hosted_service_t
{
  public:
    struct result_state_t
    {
        std::optional<scenario_route_res_t> reply;
        std::optional<std::string> error;

        scenario_route_res_t take_reply () const
        {
            if (error)
                throw std::runtime_error (*error);
            if (!reply)
                throw std::runtime_error (
                  "transient route request produced no reply");
            return *reply;
        }
    };

    transient_route_request_service_t (
      zlink::framework::app_t &app,
      scenario_route_req_t request,
      std::shared_ptr<result_state_t> result) :
        _app (app), _request (std::move (request)), _result (std::move (result))
    {
    }

    void start (zlink::framework::service_provider_t &services) override
    {
        try {
            auto &runtime =
              services.get_required<zlink::framework::route_mesh_runtime_t> ();
            const auto deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (5);
            bool ready = false;
            do {
                const auto snapshot = runtime.snapshot (route_channel);
                std::cerr << "RL-C1 route snapshot cycle=" << _request.value
                          << " state="
                          << static_cast<int> (snapshot.state);
                for (const auto &peer : snapshot.peers)
                {
                    const auto peer_ready =
                      peer.state
                      == zlink::framework::peer_state_t::ready;
                    std::cerr << " peer="
                              << peer.node_rid.to_string ()
                              << ",state="
                              << static_cast<int> (peer.state);
                    ready = ready
                            || (peer.node_rid.to_string ()
                                  == "api-a"
                                && peer_ready);
                }
                std::cerr << '\n';
                if (!ready)
                    std::this_thread::sleep_for (
                      std::chrono::milliseconds (20));
            } while (!ready && std::chrono::steady_clock::now () < deadline);
            if (!ready)
                throw std::runtime_error (
                  "transient RouteMesh did not admit api-a within 5 seconds");

            auto &routes =
              services.get_required<zlink::framework::route_client_t> ();
            _result->reply =
              routes
                .request_to_node (
                  route_channel, zlink::routing_id_t::from ("api-a"),
                  _request)
                .timeout (std::chrono::seconds (5))
                .submit<scenario_route_res_t> ()
                .result ()
                .value ();
        }
        catch (const std::exception &error) {
            std::cerr << "RL-C1 transient RouteMesh error cycle="
                      << _request.value << " error=" << error.what () << '\n';
            _result->error = error.what ();
        }
        _app.stop ();
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t &_app;
    scenario_route_req_t _request;
    std::shared_ptr<result_state_t> _result;
};

inline profile_res_t request_profile_with_new_client_host (const consumer_options_t &options,
                                                           const profile_req_t &request)
{
    const auto trace_id = request.marker.empty () ? request.value : request.marker;
    auto app = zlink::framework::app_t::create ();
    auto result = std::make_shared<transient_profile_request_service_t::result_state_t> ();
    auto service = std::make_unique<transient_profile_request_service_t> (app, request, result);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        server::add_redis_location_store (framework, options.redis_endpoint,
                                          options.redis_key_prefix);
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/storm-" + trace_id + "-flow.log")
          .trace_label ("storm-" + trace_id);
        framework.add_client_server_channel (api_channel).client ();
    });
    app.add_hosted_service (std::move (service));
    const auto exit_code = app.run (0, nullptr);
    if (exit_code != 0) {
        throw std::runtime_error ("transient profile request host failed");
    }
    return result->take_reply ();
}

inline scenario_route_res_t request_route_with_recreated_mesh_host (
  const consumer_options_t &options,
  const scenario_route_req_t &request)
{
    auto app = zlink::framework::app_t::create ();
    auto result =
      std::make_shared<transient_route_request_service_t::result_state_t> ();
    auto service = std::make_unique<transient_route_request_service_t> (
      app, request, result);
    app.add_zlink_framework (
      [&] (zlink::framework::zlink_framework_options_t &framework) {
          server::add_redis_location_store (
            framework, options.redis_endpoint, options.redis_key_prefix);
          framework.add_route_mesh (route_channel)
            .listen ("tcp://127.0.0.1:0")
            .set_routing_id (
              zlink::routing_id_t::from ("rl-c1-route-consumer"))
            .channel_name (route_channel);
      });
    app.add_hosted_service (std::move (service));
    std::cerr << "RL-C1 transient RouteMesh run begin cycle="
              << request.value << '\n';
    const auto exit_code = app.run (0, nullptr);
    std::cerr << "RL-C1 transient RouteMesh run end cycle="
              << request.value << " exit=" << exit_code << '\n';
    if (exit_code != 0)
        throw std::runtime_error ("transient RouteMesh host failed");
    return result->take_reply ();
}

class new_client_profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<consumer_options_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit new_client_profile_request_handler_t (consumer_options_t &options) :
        _options (options)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        return request_profile_with_new_client_host (_options, request);
    }

  private:
    consumer_options_t &_options;
};

class recreated_mesh_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<consumer_options_t>;
    using request_type = scenario_route_req_t;
    using reply_type = scenario_route_res_t;

    explicit recreated_mesh_request_handler_t (consumer_options_t &options) :
        _options (options)
    {
    }

    scenario_route_res_t handle (const scenario_route_req_t &request)
    {
        return request_route_with_recreated_mesh_host (_options, request);
    }

  private:
    consumer_options_t &_options;
};

} // namespace zlink::framework::e2e::resilience_lifecycle::consumer
