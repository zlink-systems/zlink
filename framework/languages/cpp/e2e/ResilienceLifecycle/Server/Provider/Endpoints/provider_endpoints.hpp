/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/evidence_store.hpp"
#include "../Infrastructure/fault_state.hpp"
#include "../Infrastructure/server_weight_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;

    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_evidence.snapshot ()).dump ();
        return response;
    }

  private:
    evidence_store_t &_evidence;
};

class server_weight_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<
      zlink::framework::channel_runtime_options_t,
      evidence_store_t,
      server_weight_state_t>;

    server_weight_handler_t (zlink::framework::channel_runtime_options_t &options,
                             evidence_store_t &evidence,
                             server_weight_state_t &state) :
        _options (options), _evidence (evidence), _state (state)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto found = request.query_values.find ("weight");
        if (found == request.query_values.end ()) {
            zlink::framework::http_response_t response;
            response.status = 400;
            response.body = R"({"error":"weight is required"})";
            return response;
        }
        const auto weight = static_cast<std::uint32_t> (std::stoul (found->second));
        set_server_weight (weight, "server-weight");
        zlink::framework::http_response_t response;
        response.body = nlohmann::json{{"weight", weight}}.dump ();
        return response;
    }

  protected:
    void set_server_weight (std::uint32_t weight, const std::string &action)
    {
        _options.client_server_channel (api_channel)
          .configure_server_socket ()
          .peer_weight (zlink::peer_weight_t::value (weight));
        _state.set (weight);
        _evidence.record ("Admin",
                          "action=" + action + ";weight=" + std::to_string (weight));
    }

  private:
    zlink::framework::channel_runtime_options_t &_options;
    evidence_store_t &_evidence;
    server_weight_state_t &_state;
};

class drain_handler_t : public server_weight_handler_t
{
  public:
    using server_weight_handler_t::server_weight_handler_t;

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        set_server_weight (0, "drain");
        zlink::framework::http_response_t response;
        response.body = R"({"status":"drained","weight":0})";
        return response;
    }
};

class restore_handler_t : public server_weight_handler_t
{
  public:
    using server_weight_handler_t::server_weight_handler_t;

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        set_server_weight (100, "restore");
        zlink::framework::http_response_t response;
        response.body = R"({"status":"restored","weight":100})";
        return response;
    }
};

class crash_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
            std::raise (SIGABRT);
        }).detach ();

        zlink::framework::http_response_t response;
        response.body = R"({"status":"crashing"})";
        return response;
    }
};

class shutdown_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
            std::raise (SIGTERM);
        }).detach ();

        zlink::framework::http_response_t response;
        response.body = R"({"status":"stopping"})";
        return response;
    }
};

class weight_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<server_weight_state_t>;

    explicit weight_handler_t (server_weight_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json{{"weight", _state.get ()}}.dump ();
        return response;
    }

  private:
    server_weight_state_t &_state;
};

class weight_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<server_weight_state_t>;

    explicit weight_wait_handler_t (server_weight_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        auto body = nlohmann::json::parse (request.body.empty () ? "{}" : request.body);
        const auto expected = body.value ("expected", _state.get ());
        const auto timeout_ms = std::clamp (body.value ("timeoutMilliseconds", 30000), 1, 30000);
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
        while (std::chrono::steady_clock::now () < deadline) {
            const auto current = _state.get ();
            if (current == static_cast<std::uint32_t> (expected)) {
                zlink::framework::http_response_t response;
                response.body = nlohmann::json{{"weight", current}}.dump ();
                return response;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        zlink::framework::http_response_t response;
        response.status = 504;
        response.body = nlohmann::json{{"error", "provider weight did not match expected"},
                                       {"expected", expected},
                                       {"weight", _state.get ()}}
                          .dump ();
        return response;
    }

  private:
    server_weight_state_t &_state;
};

class observer_fault_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<fault_state_t>;

    explicit observer_fault_handler_t (fault_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _state.set_mode ("observer-throws");
        zlink::framework::http_response_t response;
        response.body = R"({"mode":"observer-throws"})";
        return response;
    }

  private:
    fault_state_t &_state;
};

class gray_fault_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<fault_state_t>;

    explicit gray_fault_handler_t (fault_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _state.set_mode ("gray");
        zlink::framework::http_response_t response;
        response.body = R"({"mode":"gray"})";
        return response;
    }

  private:
    fault_state_t &_state;
};

class clear_fault_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<fault_state_t>;

    explicit clear_fault_handler_t (fault_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _state.set_mode ("none");
        zlink::framework::http_response_t response;
        response.body = R"({"mode":"none"})";
        return response;
    }

  private:
    fault_state_t &_state;
};

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
