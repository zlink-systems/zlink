/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline int sm_g3_count_evidence (const evidence_snapshot_t &snapshot,
                                 const std::string &marker,
                                 const std::string &actor_id,
                                 const std::string &spot_id)
{
    int count = 0;
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == marker && entry.actor_id == actor_id && entry.spot_id == spot_id) {
            ++count;
        }
    }
    return count;
}

inline evidence_snapshot_t fetch_sm_g3_evidence_until (zlink::http_client::client_t &client,
                                                       const std::array<std::string, 2> &actor_ids,
                                                       const std::string &spot_id)
{
    for (int attempt = 0; attempt < 80; ++attempt) {
        auto raw = client.get ("/evidence").submit_raw ().result ();
        if (raw && raw.value ().status < 400) {
            auto snapshot =
              nlohmann::json::parse (raw.value ().body).get<evidence_snapshot_t> ();
            bool complete = true;
            for (const auto &actor_id : actor_ids) {
                complete = complete
                           && sm_g3_count_evidence (snapshot, "ActorJoined", actor_id, spot_id)
                                == 1
                           && sm_g3_count_evidence (snapshot, "ActorLeft", actor_id, spot_id)
                                == 1;
            }
            if (complete) {
                return snapshot;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("SM-G3 expected actor join/leave evidence was not observed");
}

struct sm_g3_bound_client_t
{
    std::string actor_id;
    zlink::stream_connector::connector_t stream;
};

inline sm_g3_bound_client_t connect_sm_g3_actor (const std::string &session_stream_endpoint,
                                                 const std::string &spot_key,
                                                 const std::string &actor_id)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    auto stream = zlink::stream_connector::connector_factory_t::create (options);

    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-G3 stream connect failed for " + actor_id);
    }

    auto auth =
      stream.request (stream_ensure_auth_req_t{"play-a", actor_id, actor_id + "-display"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid != "session-a") {
        const auto detail =
          auth.error () ? auth.error ()->message : "stream auth reply mismatch";
        (void) stream.close ();
        throw std::runtime_error ("SM-G3 stream auth failed for " + actor_id + ": " + detail);
    }

    auto joined =
      stream.request (join_req_t{.key = spot_key,
                           .actor_id = actor_id,
                           .display_name = actor_id + "-display",
                           .level = 3,
                           .tags = {"stream", "SM-G3", "concurrent"}})
        .packet_name ("JoinReq")
        .metadata ("actor-id", actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<join_res_t> ();
    if (!joined || joined.value ().owner_node_rid != "play-a"
        || joined.value ().actor.actor_id != actor_id) {
        (void) stream.close ();
        throw std::runtime_error ("SM-G3 join failed for " + actor_id);
    }

    return sm_g3_bound_client_t{actor_id, std::move (stream)};
}

inline void run_sm_g3_actor_ping (zlink::stream_connector::connector_t &stream,
                                  const std::string &actor_id)
{
    auto ping = stream.request (actor_ping_req_t{actor_id})
                  .packet_name ("ActorPingReq")
                  .metadata ("actor-id", actor_id)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<actor_ping_res_t> ();
    if (!ping || ping.value ().actor_id != actor_id || ping.value ().node_rid != "play-a") {
        const auto detail =
          ping ? "reply actor_id=" + ping.value ().actor_id
                   + " node_rid=" + ping.value ().node_rid
                   + " value=" + ping.value ().value
               : ping.error () ? ping.error ()->message : "unknown stream error";
        (void) stream.close ();
        throw std::runtime_error ("SM-G3 actor request mismatch for " + actor_id + ": "
                                  + detail);
    }
}

inline void run_sm_g3_actor_leave (zlink::stream_connector::connector_t &stream,
                                   const std::string &actor_id)
{
    auto left = stream.request (leave_req_t{.actor_id = actor_id, .reason = "sm-g3"})
                  .packet_name ("LeaveReq")
                  .metadata ("actor-id", actor_id)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<leave_res_t> ();
    if (!left || !left.value ().left || left.value ().actor_id != actor_id) {
        const auto detail =
          left ? "left=" + std::string (left.value ().left ? "true" : "false")
                   + " actor_id=" + left.value ().actor_id
               : left.error () ? left.error ()->message : "unknown stream error";
        (void) stream.close ();
        throw std::runtime_error ("SM-G3 leave reply mismatch for " + actor_id + ": "
                                  + detail);
    }
}

inline void run_sm_g3_scenario (const std::string &play_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-G3");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-G3");
    }

    constexpr auto spot_key = "sm-g3-concurrent";
    constexpr auto spot_id = "user:play-a:sm-g3-concurrent";
    const std::array<std::string, 2> actor_ids{"actor-sm-g3-0", "actor-sm-g3-1"};

    std::vector<sm_g3_bound_client_t> clients;
    clients.reserve (actor_ids.size ());
    for (const auto &actor_id : actor_ids) {
        clients.push_back (connect_sm_g3_actor (session_stream_endpoint, spot_key, actor_id));
    }

    auto first = std::async (std::launch::async, [&] {
        run_sm_g3_actor_ping (clients[0].stream, clients[0].actor_id);
    });
    auto second = std::async (std::launch::async, [&] {
        run_sm_g3_actor_ping (clients[1].stream, clients[1].actor_id);
    });

    first.get ();
    second.get ();
    for (auto &client : clients) {
        run_sm_g3_actor_leave (client.stream, client.actor_id);
    }
    for (auto &client : clients) {
        (void) client.stream.close ();
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    const auto evidence = fetch_sm_g3_evidence_until (play_a, actor_ids, spot_id);
    for (const auto &actor_id : actor_ids) {
        if (sm_g3_count_evidence (evidence, "ActorJoined", actor_id, spot_id) != 1) {
            throw std::runtime_error ("SM-G3 join evidence count mismatch for " + actor_id);
        }
        if (sm_g3_count_evidence (evidence, "ActorLeft", actor_id, spot_id) != 1) {
            throw std::runtime_error ("SM-G3 leave evidence count mismatch for " + actor_id);
        }
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
