/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Client-side HTTP helpers for the Track F relocation node role
 * (Server/Relocation). Reuses the generic post_json/get_json/ensure
 * plumbing already in client_support.hpp (namespace sf_client) -- only the
 * DTOs differ, so no new low-level HTTP code is needed here. */

#include "client_support.hpp"
#include "../../Shared/relocation_contracts.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::e2e::store_failure_relocation::client
{

namespace sf_client = zlink::framework::e2e::store_failure::client;

/* A hard client-side deadline for a call whose underlying HTTP client
 * timeout has been observed NOT to bound the full request-response wait
 * (see SF-F2's post-transfer_in_failed probe). std::async's future would
 * still block its destructor until the worker thread finishes if the
 * deadline is missed, so this uses a detached std::thread instead -- a
 * missed deadline leaks one blocked thread until the client process exits
 * (which happens right after the scenario returns) rather than hanging
 * the scenario itself. */
template <typename TResult, typename TCallable>
inline TResult call_with_hard_timeout (TCallable &&callable,
                                       std::chrono::milliseconds deadline,
                                       const std::string &description)
{
    auto mutex = std::make_shared<std::mutex> ();
    auto condition = std::make_shared<std::condition_variable> ();
    auto done = std::make_shared<bool> (false);
    auto result = std::make_shared<std::optional<TResult>> ();
    auto error = std::make_shared<std::optional<std::string>> ();

    std::thread ([mutex, condition, done, result, error,
                 callable = std::forward<TCallable> (callable)] () mutable {
        std::optional<TResult> local_result;
        std::optional<std::string> local_error;
        try {
            local_result = callable ();
        }
        catch (const std::exception &ex) {
            local_error = ex.what ();
        }
        catch (...) {
            local_error = "unknown exception";
        }
        {
            std::lock_guard<std::mutex> lock (*mutex);
            *result = std::move (local_result);
            *error = std::move (local_error);
            *done = true;
        }
        condition->notify_all ();
    }).detach ();

    std::unique_lock<std::mutex> lock (*mutex);
    if (!condition->wait_for (lock, deadline, [&] { return *done; })) {
        throw std::runtime_error (description + " did not return within "
                                  + std::to_string (deadline.count ()) + "ms (hard timeout)");
    }
    if (*error)
        throw std::runtime_error (description + " failed: " + **error);
    return std::move (**result);
}

struct actor_ref_res_t
{
    std::string actor_id;
    std::string node_rid;
    std::int64_t generation = 0;
};

inline create_spot_res_t create_spot (const std::string &node_url, const std::string &spot_id)
{
    return sf_client::post_json<create_spot_req_t, create_spot_res_t> (
      node_url, "/spots", {.spot_id = spot_id});
}

inline void close_spot (const std::string &node_url, const std::string &spot_id)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (node_url)
                    .timeout (std::chrono::seconds (5))
                    .build ();
    (void) client.post ("/spots/" + spot_id + "/close").submit<nlohmann::json> ().result ().value ();
}

/* Mesh placement is hash-based, not "whichever node URL you POST to" -- a
 * create_spot(node_b_url, ...) call can still land the Spot on df-a.
 * Track F's scenarios need the Spot and Actor pinned to specific,
 * different nodes (so a relocation is a genuine cross-node transfer, not a
 * same-node short-circuit that skips capture()/restore()), so this
 * retries with a fresh id until placement matches, exactly like
 * SpotActorTransfer's create_spot_until_placed_on. */
inline create_spot_res_t create_spot_until_placed_on (const std::string &node_url,
                                                      const std::string &base_spot_id,
                                                      const std::string &target_node_rid)
{
    for (unsigned attempt = 0; attempt != 64; ++attempt) {
        const auto spot_id =
          attempt == 0 ? base_spot_id : base_spot_id + "-placement-" + std::to_string (attempt);
        auto created = create_spot (node_url, spot_id);
        if (created.node_rid == target_node_rid)
            return created;
        close_spot (node_url, created.spot_id);
    }
    throw std::runtime_error ("could not place Spot on " + target_node_rid + " after 64 attempts");
}

inline actor_create_res_t create_actor (const std::string &node_url,
                                        const std::string &actor_id,
                                        const std::string &actor_type,
                                        std::uint64_t payload_length)
{
    return sf_client::post_json<actor_create_req_t, actor_create_res_t> (
      node_url, "/actors", {.actor_id = actor_id, .actor_type = actor_type,
                            .payload_length = payload_length});
}

inline actor_create_res_t create_actor_until_placed_on (const std::string &node_url,
                                                        const std::string &base_actor_id,
                                                        const std::string &actor_type,
                                                        std::uint64_t payload_length,
                                                        const std::string &target_node_rid)
{
    for (unsigned attempt = 0; attempt != 64; ++attempt) {
        const auto actor_id =
          attempt == 0 ? base_actor_id : base_actor_id + "-placement-" + std::to_string (attempt);
        auto created = create_actor (node_url, actor_id, actor_type, payload_length);
        if (created.node_rid == target_node_rid)
            return created;
    }
    throw std::runtime_error ("could not place Actor on " + target_node_rid + " after 64 attempts");
}

inline relocate_res_t relocate (const std::string &node_url,
                                const std::string &actor_id,
                                const std::string &scenario,
                                const std::string &target_spot_id,
                                std::chrono::milliseconds timeout = std::chrono::seconds (25))
{
    return sf_client::post_json<relocate_req_t, relocate_res_t> (
      node_url, "/actors/" + actor_id + "/relocate",
      {.scenario = scenario, .target_spot_id = target_spot_id}, timeout);
}

inline probe_state_res_t probe (const std::string &node_url,
                                const std::string &actor_id,
                                const std::string &scenario,
                                const std::string &marker,
                                std::chrono::milliseconds timeout = std::chrono::seconds (10))
{
    return sf_client::post_json<probe_state_req_t, probe_state_res_t> (
      node_url, "/actors/" + actor_id + "/probe", {.scenario = scenario, .marker = marker},
      timeout);
}

inline actor_ref_res_t get_actor_ref (const std::string &node_url, const std::string &actor_id)
{
    const auto json = sf_client::get_json<nlohmann::json> (node_url, "/actors/" + actor_id + "/ref");
    return {json.value ("actorId", ""), json.value ("nodeRid", ""),
           json.value ("generation", std::int64_t{0})};
}

inline gate_release_res_t release_capture_gate (const std::string &node_url,
                                                const std::string &actor_id)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (node_url)
                    .timeout (std::chrono::seconds (5))
                    .build ();
    return client.post ("/capture-gates/" + actor_id + "/release")
      .submit<gate_release_res_t> ()
      .result ()
      .value ()
      .body;
}

inline std::vector<relocation_evidence_t> get_evidence (const std::string &node_url)
{
    return sf_client::get_json<std::vector<relocation_evidence_t>> (node_url, "/evidence");
}

inline std::vector<relocation_evidence_t>
wait_evidence (const std::string &node_url,
              const std::vector<std::string> &needles,
              std::chrono::milliseconds timeout = std::chrono::seconds (20))
{
    nlohmann::json body{{"contains", needles}};
    auto client = zlink::http_client::client_t::create ()
                    .base_url (node_url)
                    .timeout (timeout + std::chrono::seconds (5))
                    .build ();
    auto response = client.post ("/evidence/wait")
                      .body (body)
                      .submit<std::vector<relocation_evidence_t>> ()
                      .result ()
                      .value ()
                      .body;
    for (const auto &needle : needles) {
        bool found = false;
        for (const auto &entry : response) {
            if (evidence_text (entry).find (needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        sf_client::ensure (found, "expected evidence not found: " + needle);
    }
    return response;
}

} // namespace zlink::e2e::store_failure_relocation::client
