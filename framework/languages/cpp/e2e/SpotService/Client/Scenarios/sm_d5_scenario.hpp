/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d5_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D5");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    constexpr auto single_actor_id = "stream-disconnect-d5-notified-single";
    constexpr auto remote_actor_id = "stream-disconnect-d5-notified-remote";
    constexpr auto notified_actor_id = "stream-disconnect-d5-notified";
    constexpr auto muted_actor_id = "stream-disconnect-d5-muted";

    auto single_stream = zlink::stream_connector::connector_factory_t::create (options);
    auto remote_stream = zlink::stream_connector::connector_factory_t::create (options);
    auto multi_stream = zlink::stream_connector::connector_factory_t::create (options);

    if (!single_stream.connect ()) {
        throw std::runtime_error ("SM-D5 single stream connect failed");
    }
    if (!remote_stream.connect ()) {
        throw std::runtime_error ("SM-D5 remote stream connect failed");
    }
    if (!multi_stream.connect ()) {
        throw std::runtime_error ("SM-D5 multi stream connect failed");
    }

    auto single_auth =
      single_stream.request (
                     stream_ensure_auth_req_t{"play-a", single_actor_id, "SM-D5 Single"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    auto single_join =
      single_stream.request (join_req_t{.key = "a-stream-disconnect-single",
                                  .actor_id = single_actor_id,
                                  .display_name = "SM-D5 Single",
                                  .level = 5,
                                  .tags = {"stream", "SM-D5", "single"}})
        .packet_name ("JoinReq")
        .metadata ("actor-id", single_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<join_res_t> ();
    if (!single_auth || !single_join) {
        throw std::runtime_error ("SM-D5 single bind setup failed");
    }

    auto remote_auth =
      remote_stream.request (
                     stream_ensure_auth_req_t{"play-b", remote_actor_id, "SM-D5 Remote"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    auto remote_join =
      remote_stream.request (join_req_t{.key = "b-stream-disconnect-remote",
                                  .actor_id = remote_actor_id,
                                  .display_name = "SM-D5 Remote",
                                  .level = 5,
                                  .tags = {"stream", "SM-D5", "remote"}})
        .packet_name ("JoinReq")
        .metadata ("actor-id", remote_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<join_res_t> ();
    if (!remote_auth || !remote_join) {
        throw std::runtime_error ("SM-D5 remote bind setup failed");
    }

    auto notified_auth =
      multi_stream.request (
                    stream_ensure_auth_req_t{"play-a", notified_actor_id, "SM-D5 Notified"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    auto muted_auth =
      multi_stream.request (
                    stream_ensure_auth_req_t{"play-a", muted_actor_id, "SM-D5 Muted"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    auto notified_join =
      multi_stream.request (join_req_t{.key = "a-stream-disconnect-notified",
                                 .actor_id = notified_actor_id,
                                 .display_name = "SM-D5 Notified",
                                 .level = 5,
                                 .tags = {"stream", "SM-D5", "notified"}})
        .packet_name ("JoinReq")
        .metadata ("actor-id", notified_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<join_res_t> ();
    auto muted_join =
      multi_stream.request (join_req_t{.key = "a-stream-disconnect-muted",
                                 .actor_id = muted_actor_id,
                                 .display_name = "SM-D5 Muted",
                                 .level = 5,
                                 .tags = {"stream", "SM-D5", "muted"}})
        .packet_name ("JoinReq")
        .metadata ("actor-id", muted_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<join_res_t> ();
    if (!notified_auth || !muted_auth || !notified_join || !muted_join) {
        throw std::runtime_error ("SM-D5 multi bind setup failed");
    }

    (void) single_stream.close ();
    (void) remote_stream.close ();
    (void) multi_stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
