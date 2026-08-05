/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "Support/trigger_client_requests.hpp"
#include "Support/trigger_log_reader.hpp"
#include "Support/trigger_options.hpp"
#include "Support/trigger_validation.hpp"

#include "../Shared/evidence_store.hpp"
#include "../../Shared/runtime_monitoring_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring::trigger
{

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
        auto call = _channels.request (profile_channel, request)
                      .timeout (std::chrono::milliseconds (3000))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (!reply.has_value ()) {
            throw std::runtime_error (
              std::string ("trigger profile request failed: ")
              + (reply.error () ? reply.error ()->what () : "unknown"));
        }
        return reply.value ();
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class service_b_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<trigger_options_t, server::evidence_store_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    service_b_request_handler_t (const trigger_options_t &options,
                                 server::evidence_store_t &evidence) :
        _options (options), _evidence (evidence)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        try {
            auto reply = request_with_transient_host (_options, _options.service_b_channel_endpoint,
                                                     "trigger-service-b-" + request.marker,
                                                     request);
            _evidence.add ("trigger-request|target=service-b|marker=" + request.marker
                           + "|result=ok");
            return reply;
        }
        catch (const std::exception &ex) {
            _evidence.add ("trigger-request|target=service-b|marker=" + request.marker
                           + "|error=" + ex.what ());
            throw;
        }
    }

  private:
    const trigger_options_t &_options;
    server::evidence_store_t &_evidence;
};

class service_a_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<trigger_options_t, server::evidence_store_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    service_a_request_handler_t (const trigger_options_t &options,
                                 server::evidence_store_t &evidence) :
        _options (options), _evidence (evidence)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        try {
            auto reply = request_with_transient_host (_options, _options.service_channel_endpoint,
                                                     "trigger-service-a-" + request.marker,
                                                     request);
            _evidence.add ("trigger-request|target=service-a|marker=" + request.marker
                           + "|result=ok");
            return reply;
        }
        catch (const std::exception &ex) {
            _evidence.add ("trigger-request|target=service-a|marker=" + request.marker
                           + "|error=" + ex.what ());
            throw;
        }
    }

  private:
    const trigger_options_t &_options;
    server::evidence_store_t &_evidence;
};

class throwing_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<trigger_options_t, server::evidence_store_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    throwing_request_handler_t (const trigger_options_t &options,
                                server::evidence_store_t &evidence) :
        _options (options), _evidence (evidence)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        try {
            auto reply = request_with_transient_host (_options, _options.throw_channel_endpoint,
                                                     "trigger-throw-" + request.marker, request);
            _evidence.add ("trigger-request|target=throw|marker=" + request.marker + "|result=ok");
            return reply;
        }
        catch (const std::exception &ex) {
            _evidence.add ("trigger-request|target=throw|marker=" + request.marker
                           + "|error=" + ex.what ());
            throw;
        }
    }

  private:
    const trigger_options_t &_options;
    server::evidence_store_t &_evidence;
};

class throw_stderr_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<trigger_options_t>;

    explicit throw_stderr_handler_t (const trigger_options_t &options) : _options (options) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (read_log_lines (throw_stderr_path ())).dump ();
        return response;
    }

  private:
    std::string throw_stderr_path () const { return _options.log_dir + "/throw.stderr.log"; }

    const trigger_options_t &_options;
};

class throw_stderr_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<trigger_options_t>;
    using request_type = evidence_wait_req_t;
    using reply_type = std::vector<std::string>;

    explicit throw_stderr_wait_handler_t (const trigger_options_t &options) : _options (options) {}

    std::vector<std::string> handle (const evidence_wait_req_t &request)
    {
        return wait_for_log_lines (throw_stderr_path (), request);
    }

  private:
    std::string throw_stderr_path () const { return _options.log_dir + "/throw.stderr.log"; }

    const trigger_options_t &_options;
};

class duplicate_source_validation_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (verify_duplicate_socket_source ()).dump ();
        return response;
    }
};

class interval_validation_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (verify_polling_interval ()).dump ();
        return response;
    }
};

class missing_socket_validation_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (verify_missing_socket_source ()).dump ();
        return response;
    }
};

class handshake_failure_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<trigger_options_t, server::evidence_store_t>;

    handshake_failure_handler_t (const trigger_options_t &options,
                                 server::evidence_store_t &evidence) :
        _options (options), _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        send_invalid_handshake (_options.service_channel_endpoint);
        _evidence.add ("trigger-handshake-failure|target=service|result=sent");
        zlink::framework::http_response_t response;
        response.body = R"({"triggered":true})";
        return response;
    }

  private:
    const trigger_options_t &_options;
    server::evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::runtime_monitoring::trigger
