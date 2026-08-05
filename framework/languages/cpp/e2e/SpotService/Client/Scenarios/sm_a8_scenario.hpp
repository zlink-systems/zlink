/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

template <typename TReply, typename TRequest>
inline TReply post_json (zlink::http_client::client_t &api,
                         const std::string &path,
                         const TRequest &request,
                         const std::string &label)
{
    auto raw = api.post (path).body (request).submit_raw ().result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what () : label + " HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error (label + " HTTP status " + std::to_string (raw.value ().status)
                                  + ": " + raw.value ().body);
    }
    return nlohmann::json::parse (raw.value ().body).template get<TReply> ();
}

inline state_res_t post_routed_state (zlink::http_client::client_t &api,
                                      const spot_state_route_req_t &request,
                                      const std::string &label)
{
    auto raw = api.post ("/spot/state/request").body (request).submit_raw ().result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what () : label + " HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error (label + " HTTP status " + std::to_string (raw.value ().status)
                                  + ": " + raw.value ().body);
    }
    return nlohmann::json::parse (raw.value ().body).get<state_res_t> ();
}

/* 서버가 idle keep-alive 연결을 닫으면 pooled 연결의 다음 요청이 "end of stream"으로
 * 끊긴다(worker가 5초 도는 동안 play-a 연결이 놀고 있다). 전송 오류일 때만 새 연결로
 * 다시 시도한다 — HTTP 상태 오류는 그대로 실패시킨다. */
template <typename TReply, typename TRequest>
inline TReply post_json_fresh (const std::string &base_url,
                               const std::string &path,
                               const TRequest &request,
                               const std::string &label,
                               int attempts = 3)
{
    std::string transport_error;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto api = zlink::http_client::client_t::create ()
                     .base_url (base_url)
                     .timeout (std::chrono::seconds (30))
                     .build ();
        auto raw = api.post (path).body (request).submit_raw ().result ();
        if (!raw) {
            transport_error = raw.error () ? raw.error ()->what () : "transport failure";
            std::this_thread::sleep_for (std::chrono::milliseconds (200));
            continue;
        }
        if (raw.value ().status >= 400) {
            throw std::runtime_error (label + " HTTP status "
                                      + std::to_string (raw.value ().status) + ": "
                                      + raw.value ().body);
        }
        return nlohmann::json::parse (raw.value ().body).template get<TReply> ();
    }
    throw std::runtime_error (label + " HTTP failed: " + transport_error);
}

inline int find_evidence_index (const evidence_snapshot_t &snapshot,
                                const std::string &marker,
                                const std::string &spot_id,
                                const std::string &value)
{
    for (std::size_t index = 0; index < snapshot.entries.size (); ++index) {
        const auto &entry = snapshot.entries[index];
        if (entry.marker == marker && entry.spot_id == spot_id && entry.value == value) {
            return static_cast<int> (index);
        }
    }
    return -1;
}

inline void wait_for_evidence (zlink::http_client::client_t &api,
                               const std::string &marker,
                               const std::string &spot_id,
                               const std::string &value,
                               const std::string &label)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        auto raw = api.get ("/evidence").submit_raw ().result ();
        if (raw && raw.value ().status < 400) {
            const auto snapshot =
              nlohmann::json::parse (raw.value ().body).get<evidence_snapshot_t> ();
            if (find_evidence_index (snapshot, marker, spot_id, value) >= 0) {
                return;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error (label + " evidence did not appear");
}

inline void run_sm_a8_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A8");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-A8");
    }

    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();

    constexpr auto spot_key = "sm-a8-worker";
    const auto spot_id = user_spot_id_for_key (spot_key);
    constexpr auto marker = "sm-a8-worker";
    const auto joined = post_json_fresh<join_res_t> (
      play_http_endpoint, "/spot/join",
      join_req_t{.key = spot_key,
                 .actor_id = "sm-a8-actor",
                 .display_name = "SM-A8",
                 .level = 8,
                 .tags = {"worker-routing"}},
      "SM-A8 join spot");
    if (joined.spot_id != spot_id || joined.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A8 worker spot was not joined on play-a");
    }

    const auto ready = post_routed_state (
      play_b,
      spot_state_route_req_t{.target_node_rid = "play-a",
                             .spot_id = spot_id,
                             .state = state_req_t{.op = "add", .amount = 0}},
      "SM-A8 ready state");
    if (ready.spot_id != spot_id || ready.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A8 worker spot route did not become ready");
    }

    /* The worker must still be running when the same-spot request lands.
     * SM-A8 uses the explicit yield terminator, so the Spot turn is available.
     * The HTTP client performs a request synchronously on the calling thread,
     * so the start call gets its own thread and its own client — exactly the
     * concurrency the .NET SM-A8 reference expresses with an un-awaited task. */
    std::optional<spot_worker_start_res_t> worker;
    std::optional<std::string> worker_error;
    std::thread worker_thread ([&] {
        try {
            worker = post_json_fresh<spot_worker_start_res_t> (
              play_http_endpoint, "/spot/worker/start",
              spot_worker_start_req_t{
                .spot_id = spot_id, .marker = marker, .delay_ms = 5000},
              "SM-A8 worker start");
        }
        catch (const std::exception &error) {
            worker_error = error.what ();
        }
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (500));

    /* A pooled keep-alive connection can be closed under the concurrent load of
     * the in-flight worker call; the routed state request is idempotent enough
     * to retry on a transport error (a fresh client opens a new connection). */
    std::optional<state_res_t> during_reply;
    std::string during_error;
    for (int attempt = 0; attempt < 3 && !during_reply; attempt++) {
        try {
            auto state_api = zlink::http_client::client_t::create ()
                               .base_url (play_b_http_endpoint)
                               .timeout (std::chrono::seconds (10))
                               .build ();
            during_reply = post_routed_state (
              state_api,
              spot_state_route_req_t{.target_node_rid = "play-a",
                                     .spot_id = spot_id,
                                     .state = state_req_t{.op = "add", .amount = 1}},
              "SM-A8 concurrent state");
        }
        catch (const std::exception &error) {
            during_error = error.what ();
            std::this_thread::sleep_for (std::chrono::milliseconds (200));
        }
    }
    worker_thread.join ();
    if (!during_reply) {
        throw std::runtime_error ("SM-A8 concurrent state failed: " + during_error);
    }
    const auto during = *during_reply;
    if (worker_error) {
        throw std::runtime_error (*worker_error);
    }
    if (during.value != 1) {
        throw std::runtime_error ("SM-A8 same-spot request was blocked by worker");
    }
    if (!worker || worker->spot_id != spot_id || worker->owner_node_rid != "play-a"
        || worker->marker != marker) {
        throw std::runtime_error ("SM-A8 worker start reply mismatch");
    }

    const auto completed = post_json_fresh<spot_worker_complete_res_t> (
      play_http_endpoint, "/spot/worker/complete",
      spot_worker_complete_req_t{.spot_id = spot_id, .marker = marker},
      "SM-A8 worker complete");
    if (!completed.completed || completed.spot_id != spot_id || completed.marker != marker) {
        throw std::runtime_error ("SM-A8 worker completion reply mismatch");
    }

    const auto worker_start_index =
      find_evidence_index (completed.evidence, "WorkerStarted", spot_id, marker);
    const auto state_index = find_evidence_index (completed.evidence, "StateRouted", spot_id, "1");
    const auto worker_complete_index =
      find_evidence_index (completed.evidence, "WorkerCompleted", spot_id, marker);
    if (worker_start_index < 0 || state_index <= worker_start_index
        || worker_complete_index <= state_index) {
        throw std::runtime_error ("SM-A8 worker evidence order mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
