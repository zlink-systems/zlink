/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../support/spot_close_scenario_fixture.hpp"

using namespace zlink::framework::test::spot_close;
using namespace std::chrono_literals;
namespace zf = zlink::framework;

TEST (ZLinkFrameworkSpotCloseConformance, DeferredJoinFailureContract)
{
    std::ifstream input (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
    const auto fixture = nlohmann::json::parse (input);
    for (const auto &row : fixture.at ("joinFailureCases"))
        run_and_check (nlohmann::json{{"name", row.at ("name")},
                                      {"given",
                                       {{"authority", "Ready"},
                                        {"onClosing", "blocks"},
                                        {"target", row.at ("target")},
                                        {"warmRoute", row.at ("warmRoute")}}},
                                      {"act", "deferredJoinFailureContract"},
                                      {"expect", {{"joinResult", row.at ("errorKind")}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ClosingResolutionSurvivesActivationRemoval)
{
    run_and_check (nlohmann::json{{"name", "closing-resolution-after-activation-removal"},
                                  {"given",
                                   {{"authority", "Ready"},
                                    {"onClosing", "blocks"},
                                    {"target", "closing"},
                                    {"warmRoute", false},
                                    {"removeAfterResolution", true}}},
                                  {"act", "deferredJoinFailureContract"},
                                  {"expect", {{"joinResult", "NotFound"}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, UnavailableJoinInvalidatesRouteForNextJoin)
{
    run_and_check (
      nlohmann::json{{"name", "unavailable-join-invalidates-route"},
                     {"given",
                      {{"authority", "Ready"},
                       {"onClosing", "blocks"},
                       {"target", "absent"},
                       {"warmRoute", true},
                       {"repeatAfterFailure", true}}},
                     {"act", "deferredJoinFailureContract"},
                     {"expect", {{"joinResult", "Unavailable"}, {"nextJoinResult", "NotFound"}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, LocalCloseInvalidatesWarmJoinRoute)
{
    run_and_check (nlohmann::json{{"name", "local-close-invalidates-warm-join-route"},
                                  {"given",
                                   {{"authority", "Ready"},
                                    {"onClosing", "blocks"},
                                    {"target", "closed"},
                                    {"warmRoute", true}}},
                                  {"act", "deferredJoinFailureContract"},
                                  {"expect", {{"joinResult", "NotFound"}}}});
}
