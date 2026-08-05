/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d9_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D9");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    std::mutex observations_mutex;
    std::vector<zlink::stream_connector::inbound_observation_t> observations;

    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    auto observer = stream.observe_inbound (
      [&] (const zlink::stream_connector::inbound_observation_t &observation) {
          std::lock_guard lock (observations_mutex);
          observations.push_back (observation);
      });
    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-D9 stream connect failed");
    }

    constexpr auto actor_id = "actor-sm-d9";
    auto auth =
      stream.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D9 Observer"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error (
          std::string ("SM-D9 stream auth failed: ")
          + (auth.error () ? auth.error ()->message : "unknown stream auth error"));
    }
    if (auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error (
          "SM-D9 stream auth reply mismatch: actor=" + auth.value ().actor.actor_id
          + " session=" + auth.value ().session_node_rid);
    }

    auto first = stream.request (actor_ping_req_t{"observer-1"})
                   .packet_name ("ActorPingReq")
                   .timeout (std::chrono::milliseconds (3000))
                   .submit<actor_ping_res_t> ();
    auto second = stream.request (actor_ping_req_t{"observer-2"})
                    .packet_name ("ActorPingReq")
                    .timeout (std::chrono::milliseconds (3000))
                    .submit<actor_ping_res_t> ();
    auto describe_ping = [] (const actor_ping_res_t &reply) {
        return "actor=" + reply.actor_id + "|node=" + reply.node_rid
               + "|spot=" + reply.spot_id + "|value=" + reply.value
               + "|seen=" + std::to_string (reply.seen);
    };
    if (!first || first.value ().actor_id != actor_id || first.value ().value != "observer-1") {
        throw std::runtime_error (
          std::string ("SM-D9 first ping reply mismatch: ")
          + (first ? describe_ping (first.value ())
                   : (first.error () ? first.error ()->message : "unknown error")));
    }
    if (!second || second.value ().actor_id != actor_id || second.value ().value != "observer-2") {
        throw std::runtime_error (
          std::string ("SM-D9 second ping reply mismatch: ")
          + (second ? describe_ping (second.value ())
                    : (second.error () ? second.error ()->message : "unknown error")));
    }

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    for (;;) {
        {
            std::lock_guard lock (observations_mutex);
            if (observations.size () >= 3) {
                break;
            }
        }
        if (std::chrono::steady_clock::now () >= deadline) {
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    {
        std::lock_guard lock (observations_mutex);
        auto summary = [&] {
            std::ostringstream stream_summary;
            for (const auto &observation : observations) {
                if (stream_summary.tellp () > 0) {
                    stream_summary << ",";
                }
                stream_summary << observation.name << "#"
                               << static_cast<int> (observation.kind) << "#"
                               << (observation.request_seq
                                     ? std::to_string (*observation.request_seq)
                                     : std::string ("none"));
            }
            return stream_summary.str ();
        };
        /* stream connector §5.2: Response는 request의 packet name을 그대로 되돌린다. */
        auto has_response = [&] (std::uint64_t request_seq, std::string_view packet_name) {
            for (const auto &observation : observations) {
                if (observation.kind == zlink::stream_connector::message_kind_t::response
                    && observation.name == packet_name
                    && observation.request_seq == request_seq) {
                    return true;
                }
            }
            return false;
        };
        if (!has_response (1, "StreamEnsureAuthReq")) {
            throw std::runtime_error ("SM-D9 auth response observation missing: " + summary ());
        }
        if (!has_response (2, "ActorPingReq")) {
            throw std::runtime_error ("SM-D9 first ping response observation missing: " + summary ());
        }
        if (!has_response (3, "ActorPingReq")) {
            throw std::runtime_error ("SM-D9 second ping response observation missing: " + summary ());
        }
    }

    observer.close ();
    (void) stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
