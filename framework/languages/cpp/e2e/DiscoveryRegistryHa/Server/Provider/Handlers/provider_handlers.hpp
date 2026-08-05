/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/provider_evidence_store.hpp"
#include "../Infrastructure/provider_lifecycle_control.hpp"
#include "../../Shared/runtime_status_projection.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <csignal>
#include <nlohmann/json.hpp>
#include <thread>

namespace zlink::framework::e2e::store_failure::provider
{

class profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<provider_evidence_store_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit profile_request_handler_t (provider_evidence_store_t &evidence) : _evidence (evidence)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        const auto provider_rid = _evidence.snapshot ().provider_rid;
        _evidence.record (request.marker, "rid=" + provider_rid + ";value=" + request.value);
        return {.value = "profile:" + request.value,
                .provider_rid = provider_rid,
                .marker = request.marker};
    }

  private:
    provider_evidence_store_t &_evidence;
};

class evidence_snapshot_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<provider_evidence_store_t>;

    explicit evidence_snapshot_handler_t (provider_evidence_store_t &evidence) :
        _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        nlohmann::json body = _evidence.snapshot ();
        response.body = body.dump ();
        return response;
    }

  private:
    provider_evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<provider_evidence_store_t>;
    using request_type = evidence_wait_req_t;
    using reply_type = evidence_snapshot_t;

    explicit evidence_wait_handler_t (provider_evidence_store_t &evidence) : _evidence (evidence) {}

    evidence_snapshot_t handle (const evidence_wait_req_t &request)
    {
        const auto timeout = std::chrono::milliseconds (
          request.timeout_milliseconds <= 0 ? 1 : request.timeout_milliseconds);
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        while (std::chrono::steady_clock::now () < deadline) {
            if (_evidence.contains (request.contains)) {
                return _evidence.snapshot ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        throw std::runtime_error ("timed out waiting for provider evidence");
    }

  private:
    provider_evidence_store_t &_evidence;
};

class query_status_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::location_runtime_query_t>;

    explicit query_status_handler_t (zlink::framework::location_runtime_query_t &query) :
        _query (query)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        const auto status = _query.get_status ().result ().value ();
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (server::project_runtime_status (status)).dump ();
        return response;
    }

  private:
    zlink::framework::location_runtime_query_t &_query;
};

class shutdown_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<provider_lifecycle_control_t>;

    explicit shutdown_handler_t (provider_lifecycle_control_t &lifecycle) :
        _lifecycle (lifecycle)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _lifecycle.request_stop_after_response ();

        zlink::framework::http_response_t response;
        response.body = R"({"status":"stopping"})";
        return response;
    }

  private:
    provider_lifecycle_control_t &_lifecycle;
};

class drain_handler_t
{
  public:
    using request_type = operation_status_t;
    using reply_type = operation_status_t;
    using dependency_types =
      zlink::framework::dependency_list_t<provider_lifecycle_control_t>;

    explicit drain_handler_t (provider_lifecycle_control_t &lifecycle) :
        _lifecycle (lifecycle)
    {
    }

    operation_status_t handle (const operation_status_t &)
    {
        return _lifecycle.drain_and_stop (std::chrono::seconds (30));
    }

  private:
    provider_lifecycle_control_t &_lifecycle;
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

} // namespace zlink::framework::e2e::store_failure::provider
