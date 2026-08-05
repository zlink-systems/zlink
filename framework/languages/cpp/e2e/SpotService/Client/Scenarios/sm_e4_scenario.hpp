/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline std::uint64_t sm_e4_extract_uint64 (std::string_view text, std::string_view key)
{
    const auto prefix = std::string (key) + "=";
    const auto begin = text.find (prefix);
    if (begin == std::string_view::npos) {
        throw std::runtime_error ("SM-E4 evidence field missing: " + std::string (key));
    }
    const auto value_begin = begin + prefix.size ();
    const auto value_end = text.find ('|', value_begin);
    const auto value =
      value_end == std::string_view::npos
        ? std::string (text.substr (value_begin))
        : std::string (text.substr (value_begin, value_end - value_begin));
    return static_cast<std::uint64_t> (std::stoull (value));
}

struct sm_e4_policy_evidence_t
{
    int count = 0;
    bool any_skipped = false;
    bool delay_policy_kept_schedule = true;
};

inline sm_e4_policy_evidence_t
sm_e4_collect_policy_evidence (const evidence_snapshot_t &snapshot,
                               const std::string &spot_id,
                               const std::string &name)
{
    sm_e4_policy_evidence_t evidence;
    for (const auto &entry : snapshot.entries) {
        if (entry.marker != "SpotTimerOverrun" || entry.spot_id != spot_id
            || entry.value.rfind (name + "|", 0) != 0) {
            continue;
        }

        ++evidence.count;
        const auto skipped = sm_e4_extract_uint64 (entry.value, "skipped");
        const auto delivery = sm_e4_extract_uint64 (entry.value, "delivery");
        const auto scheduled = sm_e4_extract_uint64 (entry.value, "scheduled");
        evidence.any_skipped = evidence.any_skipped || skipped > 0;
        evidence.delay_policy_kept_schedule =
          evidence.delay_policy_kept_schedule && skipped == 0 && delivery == scheduled;
    }
    return evidence;
}

inline evidence_snapshot_t sm_e4_wait_for_evidence (zlink::http_client::client_t &play_a,
                                                    const std::string &spot_id,
                                                    const std::string &name)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    do {
        auto raw = play_a.get ("/evidence").submit_raw ().result ();
        if (!raw) {
            throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                                   : "SM-E4 evidence HTTP failed");
        }
        if (raw.value ().status >= 400) {
            throw std::runtime_error ("SM-E4 evidence HTTP status "
                                      + std::to_string (raw.value ().status) + ": "
                                      + raw.value ().body);
        }

        auto snapshot = nlohmann::json::parse (raw.value ().body).get<evidence_snapshot_t> ();
        if (sm_e4_collect_policy_evidence (snapshot, spot_id, name).count >= 3) {
            return snapshot;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    } while (std::chrono::steady_clock::now () < deadline);

    throw std::runtime_error ("SM-E4 overrun evidence missing for " + name);
}

inline void run_sm_e4_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-E4");
    }

    struct policy_case_t
    {
        std::string policy;
        std::string spot_id;
        std::string name;
    };

    const std::vector<policy_case_t> cases{
      {"SkipLateTicks", "user:play-a:sm-e4-skip", "sm-e4-SkipLateTicks"},
      {"CatchUpBounded", "user:play-a:sm-e4-catch", "sm-e4-CatchUpBounded"},
      {"DelayNextTick", "user:play-a:sm-e4-delay", "sm-e4-DelayNextTick"},
    };

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();

    bool skip_late_ticks_skipped = false;
    bool catch_up_bounded_skipped = false;
    bool delay_next_tick_stayed_on_schedule = false;

    for (const auto &item : cases) {
        auto started =
          play_a.post ("/spot/overrun/start")
            .body (spot_overrun_start_req_t{.spot_id = item.spot_id,
                                            .name = item.name,
                                            .policy = item.policy,
                                            .period_ms = 25})
            .submit_raw ()
            .result ();
        if (!started) {
            throw std::runtime_error (started.error () ? started.error ()->what ()
                                                       : "SM-E4 overrun start HTTP failed");
        }
        if (started.value ().status >= 400) {
            throw std::runtime_error ("SM-E4 overrun start HTTP status "
                                      + std::to_string (started.value ().status) + ": "
                                      + started.value ().body);
        }

        const auto reply =
          nlohmann::json::parse (started.value ().body).get<spot_overrun_start_res_t> ();
        if (!reply.started || reply.spot_id != item.spot_id || reply.name != item.name) {
            throw std::runtime_error ("SM-E4 overrun start reply mismatch");
        }

        const auto snapshot = sm_e4_wait_for_evidence (play_a, item.spot_id, item.name);
        const auto evidence =
          sm_e4_collect_policy_evidence (snapshot, item.spot_id, item.name);
        if (item.policy == "SkipLateTicks") {
            skip_late_ticks_skipped = evidence.any_skipped;
        } else if (item.policy == "CatchUpBounded") {
            catch_up_bounded_skipped = evidence.any_skipped;
        } else if (item.policy == "DelayNextTick") {
            delay_next_tick_stayed_on_schedule = evidence.delay_policy_kept_schedule;
        }
    }

    if (!skip_late_ticks_skipped) {
        throw std::runtime_error ("SM-E4 SkipLateTicks did not skip late ticks");
    }
    if (!catch_up_bounded_skipped) {
        throw std::runtime_error ("SM-E4 CatchUpBounded did not show bounded catch-up");
    }
    if (!delay_next_tick_stayed_on_schedule) {
        throw std::runtime_error ("SM-E4 DelayNextTick did not stay on schedule");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
