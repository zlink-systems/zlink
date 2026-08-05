/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Spots/play_spot_types.hpp"
#include "../Support/play_support.hpp"
#include "../../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline void
deactivate_await_timer (std::map<std::string, await_timer_state_t> &timers,
                        std::mutex &timer_mutex,
                        const std::string &timer_name)
{
    std::lock_guard lock (timer_mutex);
    if (auto found = timers.find (timer_name); found != timers.end ()) {
        found->second.active = false;
    }
}

inline std::optional<await_timer_tick_state_t>
next_await_timer_tick (std::map<std::string, await_timer_state_t> &timers,
                       std::mutex &timer_mutex,
                       const std::string &timer_name)
{
    std::lock_guard lock (timer_mutex);
    auto found = timers.find (timer_name);
    if (found == timers.end ()) {
        return std::nullopt;
    }
    auto &state = found->second;
    if (!state.active) {
        return std::nullopt;
    }
    ++state.tick_count;
    return await_timer_tick_state_t{.request_id = state.request_id,
                                    .timer_name = state.timer_name,
                                    .mode = state.mode,
                                    .delay_ms = state.delay_ms,
                                    .tick_number = state.tick_count};
}

inline void
handle_timer_start_command (zlink::framework::spot_context_t &context,
                            evidence_store_t &evidence,
                            std::map<std::string, await_timer_state_t> &timers,
                            std::mutex &timer_mutex,
                            const yd::timer_start_msg_t &request)
{
    const auto spot_id = context.spot_id ();
    {
        std::lock_guard lock (timer_mutex);
        if (timers.find (request.timer_name) != timers.end ()) {
            evidence.add ("timer-start-duplicate-ignored|rid=" + evidence.node_rid
                          + "|spot=" + spot_id + "|request=" + request.request_id
                          + "|timer=" + request.timer_name + "|mode=" + request.mode);
            return;
        }
        timers.emplace (request.timer_name,
                        await_timer_state_t{.request_id = request.request_id,
                                            .timer_name = request.timer_name,
                                            .mode = request.mode,
                                            .delay_ms = request.delay_ms});
    }

    zlink::framework::timer_options_t options;
    options.overrun_policy = zlink::framework::timer_overrun_policy_t::delay_next_tick;
    auto timer = context.add_timer<await_timer_handler_t> (
      request.timer_name, std::chrono::milliseconds (request.period_ms), options);
    {
        std::lock_guard lock (timer_mutex);
        if (auto found = timers.find (request.timer_name); found != timers.end ()) {
            found->second.timer = std::move (timer);
        }
    }
    evidence.add ("timer-started|rid=" + evidence.node_rid + "|spot=" + spot_id
                  + "|request=" + request.request_id + "|timer=" + request.timer_name
                  + "|mode=" + request.mode);
}

inline void
handle_timer_stop_command (std::map<std::string, await_timer_state_t> &timers,
                           std::mutex &timer_mutex,
                           const yd::timer_stop_msg_t &request)
{
    std::vector<zlink::framework::timer_t> timers_to_cancel;
    {
        std::lock_guard lock (timer_mutex);
        for (auto it = timers.begin (); it != timers.end ();) {
            if (it->second.request_id == request.request_id) {
                timers_to_cancel.push_back (std::move (it->second.timer));
                it = timers.erase (it);
            } else {
                ++it;
            }
        }
    }
    for (auto &timer : timers_to_cancel) {
        timer.cancel ();
    }
}

inline zlink::framework::task_t<void>
handle_timer_tick (zlink::framework::spot_context_t &context,
                   evidence_store_t &evidence,
                   std::map<std::string, await_timer_state_t> &timers,
                   std::mutex &timer_mutex,
                   const zlink::framework::timer_tick_t &tick)
{
    auto state = next_await_timer_tick (timers, timer_mutex, tick.name);
    if (!state) {
        co_return;
    }

    const auto spot_id = context.spot_id ();
    const auto tick_id = std::to_string (state->tick_number);
    if (state->mode == "fast") {
        evidence.add ("timer-fast-started|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        evidence.add ("timer-fast-completed|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        deactivate_await_timer (timers, timer_mutex, state->timer_name);
        co_return;
    }

    if (state->tick_number == 1
        && (state->mode == "await-on-first" || state->mode == "await-then-next")) {
        evidence.add ("timer-await-started|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        auto call =
          context.outbound ()
            .request (yd::delay_channel,
                      yd::delay_req_t{.request_id = state->request_id,
                                      .delay_ms = state->delay_ms,
                                      .marker = state->timer_name})
            .timeout (std::chrono::milliseconds (5000));
        evidence.add ("timer-await-released|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        co_await call.submit<yd::delay_res_t> ();
        evidence.add ("timer-await-resumed|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        evidence.add ("timer-await-completed|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        if (state->mode == "await-on-first") {
            deactivate_await_timer (timers, timer_mutex, state->timer_name);
        }
        co_return;
    }

    if (state->mode == "await-then-next" && state->tick_number == 2) {
        evidence.add ("timer-next-started|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        evidence.add ("timer-next-completed|rid=" + evidence.node_rid + "|spot="
                      + spot_id + "|request=" + state->request_id + "|timer="
                      + state->timer_name + "|tick=" + tick_id + "|handler=timer");
        deactivate_await_timer (timers, timer_mutex, state->timer_name);
    }
    co_return;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
