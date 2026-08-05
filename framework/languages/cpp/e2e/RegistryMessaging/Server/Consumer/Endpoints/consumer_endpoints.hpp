/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/registry_messaging_contracts.hpp"
#include "../../Shared/public_error_type.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace zlink::framework::e2e::registry_messaging::consumer
{

inline profile_res_t request_profile (zlink::framework::channel_client_t &channels,
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
    if (reply.error ()) {
        throw *reply.error ();
    }
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::internal_failure,
      "profile request failed");
}

inline payload_res_t request_payload (zlink::framework::channel_client_t &channels,
                                      const payload_req_t &request)
{
    auto call = channels.request (api_channel, request)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<payload_res_t> ();
    const auto &reply = call.result ();
    if (reply) {
        return reply.value ();
    }
    if (reply.error ()) {
        throw *reply.error ();
    }
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::internal_failure,
      "payload request failed");
}

inline workflow_res_t request_workflow (
  zlink::framework::channel_client_t &channels,
  const workflow_req_t &request)
{
    auto call = channels.request (workflow_channel, request)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<workflow_res_t> ();
    const auto &reply = call.result ();
    if (reply)
        return reply.value ();
    if (reply.error ()) {
        throw *reply.error ();
    }
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::internal_failure,
      "workflow request failed");
}

class batch_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = std::vector<profile_req_t>;
    using reply_type = std::vector<profile_res_t>;

    explicit batch_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    std::vector<profile_res_t> handle (const std::vector<profile_req_t> &requests)
    {
        std::vector<profile_res_t> replies;
        replies.reserve (requests.size ());
        for (const auto &request : requests) {
            replies.push_back (
              request_profile (_channels, request, std::chrono::milliseconds (3000)));
        }
        return replies;
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

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
        return request_profile (_channels, request, std::chrono::milliseconds (3000));
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

/* The scale-in barrier keeps the public framework error kind inside a typed
 * E2E response.  The HTTP client intentionally treats a non-2xx status as a
 * transport-level failure, so this test-only endpoint lets the scenario
 * inspect the framework result without decoding an HTTP error body. */
class scale_in_transition_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = request_failure_res_t;

    explicit scale_in_transition_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    request_failure_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request (api_channel, request)
                      .timeout (std::chrono::seconds (4))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (reply) {
            return {.failed = false, .error_type = ""};
        }
        return {.failed = true, .error_type = public_error_type (reply)};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class workflow_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::channel_client_t>;
    using request_type = workflow_req_t;
    using reply_type = workflow_res_t;

    explicit workflow_request_handler_t (
      zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    workflow_res_t handle (const workflow_req_t &request)
    {
        return request_workflow (_channels, request);
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
                .error_type = public_error_type (reply)};
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
                .error_type = public_error_type (reply)};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class missing_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_msg_t;
    using reply_type = operation_status_t;

    explicit missing_command_handler_t (zlink::framework::channel_client_t &channels) :
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

class payload_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = payload_req_t;
    using reply_type = payload_res_t;

    explicit payload_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    payload_res_t handle (const payload_req_t &request)
    {
        return request_payload (_channels, request);
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class backpressure_reset_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = R"({"status":"ready"})";
        return response;
    }
};

class backpressure_send_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_msg_t;
    using reply_type = backpressure_send_res_t;

    explicit backpressure_send_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    backpressure_send_res_t handle (const profile_msg_t &command)
    {
        _channels.send (api_channel, command).submit ();
        return {.outcome = "Submitted"};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

} // namespace zlink::framework::e2e::registry_messaging::consumer
