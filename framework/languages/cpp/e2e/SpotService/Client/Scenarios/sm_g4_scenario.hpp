/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline stream_auth_res_t sm_g4_auth_stream (zlink::stream_connector::connector_t &stream,
                                            const std::string &actor_id)
{
    auto auth =
      stream.request (stream_ensure_auth_req_t{"play-a", actor_id, actor_id + "-display"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid != "session-a") {
        const auto detail =
          auth.error () ? auth.error ()->message : "stream auth reply mismatch";
        throw std::runtime_error ("SM-G4 stream auth failed for " + actor_id + ": " + detail);
    }
    return auth.value ();
}

inline void sm_g4_push_and_verify (zlink::stream_connector::connector_t &stream, int index)
{
    const auto actor_id = "actor-sm-g4-" + std::to_string (index);
    const auto value = "push-" + std::to_string (index);
    auto wait = stream.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
                  .to_future ("SM-G4 push notify missing for " + actor_id);
    auto pushed = stream.request (actor_push_req_t{value})
                    .packet_name ("PushReq")
                    .metadata ("actor-id", actor_id)
                    .timeout (std::chrono::milliseconds (3000))
                    .submit<actor_push_res_t> ();
    if (!pushed || !pushed.value ().pushed || pushed.value ().actor_id != actor_id) {
        throw std::runtime_error ("SM-G4 push reply mismatch for " + actor_id);
    }

    auto notify = wait.get ();
    if (notify.actor_id != actor_id || notify.value != value) {
        throw std::runtime_error ("SM-G4 push notify mismatch for " + actor_id);
    }
}

inline void run_sm_g4_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-G4");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    std::vector<std::unique_ptr<zlink::stream_connector::connector_t>> streams;
    for (int index = 0; index < 6; ++index) {
        auto stream =
          std::make_unique<zlink::stream_connector::connector_t> (
            zlink::stream_connector::connector_factory_t::create (options));
        auto connected = stream->connect ();
        if (!connected) {
            throw std::runtime_error ("SM-G4 stream connect failed for actor-sm-g4-"
                                      + std::to_string (index));
        }
        (void) sm_g4_auth_stream (*stream, "actor-sm-g4-" + std::to_string (index));
        streams.push_back (std::move (stream));
    }

    for (int index = 0; index < static_cast<int> (streams.size ()); ++index) {
        sm_g4_push_and_verify (*streams[static_cast<std::size_t> (index)], index);
    }
    for (auto &stream : streams) {
        (void) stream->close ();
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
