/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/spot_lifecycle_order_context.hpp"
#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <algorithm>
#include <array>
#include <future>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_a2_scenario (const std::string &play_http_endpoint,
                                spot_lifecycle_order_context_t &context)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A2");
    }

    auto api = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto observed =
      api.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"StateRouted", context.spot_id,
                           std::to_string (context.current_value)}})
        .submit_raw ()
        .result ();
    if (!observed) {
        throw std::runtime_error (observed.error () ? observed.error ()->what ()
                                                    : "SM-A2 evidence wait HTTP failed");
    }
    if (observed.value ().status >= 400) {
        throw std::runtime_error ("SM-A2 evidence wait HTTP status "
                                  + std::to_string (observed.value ().status) + ": "
                                  + observed.value ().body);
    }
    const auto evidence =
      nlohmann::json::parse (observed.value ().body).get<evidence_snapshot_t> ();
    const auto expected_value = std::to_string (context.current_value);
    const auto found = std::any_of (evidence.entries.begin (), evidence.entries.end (),
                                    [&] (const evidence_entry_t &entry) {
                                        return entry.marker == "StateRouted"
                                               && entry.spot_id == context.spot_id
                                               && entry.value == expected_value;
                                    });
    if (!found) {
        throw std::runtime_error ("SM-A2 state mutation evidence mismatch");
    }
}

inline void run_sm_a2_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A2");
    }

    auto api = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto joined =
      api.post ("/spot/join")
        .body (join_req_t{.key = "a-room",
                          .actor_id = "alice",
                          .display_name = "Alice",
                          .level = 7,
                          .tags = {"alpha", "local"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                  : "SM-A2 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-A2 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    auto created = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
    if (created.spot_id != "user:play-a:a-room") {
        throw std::runtime_error ("SM-A2 spot id mismatch: " + created.spot_id);
    }

    auto first = api.post ("/spot/state")
                   .body (spot_state_req_t{.actor_id = "alice",
                                           .state = state_req_t{.op = "add", .amount = 3}})
                   .submit_raw ()
                   .result ();
    if (!first) {
        throw std::runtime_error (first.error () ? first.error ()->what ()
                                                 : "SM-A2 first state HTTP failed");
    }
    if (first.value ().status >= 400) {
        throw std::runtime_error ("SM-A2 first state HTTP status "
                                  + std::to_string (first.value ().status) + ": "
                                  + first.value ().body);
    }
    auto state1 = nlohmann::json::parse (first.value ().body).get<state_res_t> ();

    auto second = api.post ("/spot/state")
                    .body (spot_state_req_t{.actor_id = "alice",
                                            .state = state_req_t{.op = "add", .amount = 4}})
                    .submit_raw ()
                    .result ();
    if (!second) {
        throw std::runtime_error (second.error () ? second.error ()->what ()
                                                  : "SM-A2 second state HTTP failed");
    }
    if (second.value ().status >= 400) {
        throw std::runtime_error ("SM-A2 second state HTTP status "
                                  + std::to_string (second.value ().status) + ": "
                                  + second.value ().body);
    }
    auto state2 = nlohmann::json::parse (second.value ().body).get<state_res_t> ();
    if (state1.value != 3 || state1.sequence != 1) {
        throw std::runtime_error ("SM-A2 first mutation mismatch");
    }
    if (state2.value != 7 || state2.sequence != 2) {
        throw std::runtime_error ("SM-A2 second mutation mismatch");
    }

    auto concurrent_first = std::async (std::launch::async, [play_http_endpoint] {
        auto client = zlink::http_client::client_t::create ()
                        .base_url (play_http_endpoint)
                        .build ();
        auto raw = client.post ("/spot/state")
                     .body (spot_state_req_t{.actor_id = "alice",
                                             .state = state_req_t{.op = "add", .amount = 5}})
                     .submit_raw ()
                     .result ();
        if (!raw) {
            throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                                   : "SM-A2 concurrent state HTTP failed");
        }
        if (raw.value ().status >= 400) {
            throw std::runtime_error ("SM-A2 concurrent state HTTP status "
                                      + std::to_string (raw.value ().status) + ": "
                                      + raw.value ().body);
        }
        return nlohmann::json::parse (raw.value ().body).get<state_res_t> ();
    });
    auto concurrent_second = std::async (std::launch::async, [play_http_endpoint] {
        auto client = zlink::http_client::client_t::create ()
                        .base_url (play_http_endpoint)
                        .build ();
        auto raw = client.post ("/spot/state")
                     .body (spot_state_req_t{.actor_id = "alice",
                                             .state = state_req_t{.op = "add", .amount = 6}})
                     .submit_raw ()
                     .result ();
        if (!raw) {
            throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                                   : "SM-A2 concurrent state HTTP failed");
        }
        if (raw.value ().status >= 400) {
            throw std::runtime_error ("SM-A2 concurrent state HTTP status "
                                      + std::to_string (raw.value ().status) + ": "
                                      + raw.value ().body);
        }
        return nlohmann::json::parse (raw.value ().body).get<state_res_t> ();
    });

    auto state3 = concurrent_first.get ();
    auto state4 = concurrent_second.get ();
    std::array<int, 2> concurrent_sequences{state3.sequence, state4.sequence};
    std::array<int, 2> concurrent_values{state3.value, state4.value};
    std::sort (concurrent_sequences.begin (), concurrent_sequences.end ());
    std::sort (concurrent_values.begin (), concurrent_values.end ());
    if (concurrent_sequences != std::array<int, 2>{3, 4}) {
        throw std::runtime_error ("SM-A2 concurrent mutation sequence mismatch");
    }
    if (concurrent_values[1] != 18 || (concurrent_values[0] != 12 && concurrent_values[0] != 13)) {
        throw std::runtime_error ("SM-A2 concurrent mutation value mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
