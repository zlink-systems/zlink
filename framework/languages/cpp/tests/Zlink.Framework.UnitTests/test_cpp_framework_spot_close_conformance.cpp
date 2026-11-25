/* SPDX-License-Identifier: Elastic-2.0 */
// Runs framework/runtime/conformance/spot-close-v1.json against an in-process
// app with a Location Store (Spot address messaging §7, §9; gate §7).

#include "../support/committed_authority_fixture.hpp"
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

namespace
{
using namespace zlink::framework::test::spot_close;
using namespace std::chrono_literals;
namespace zf = zlink::framework;

TEST (ZLinkFrameworkSpotCloseConformance, ManagerCloseUsesExistingEntryDeadline)
{
    run_and_check (nlohmann::json{
      {"name", "ManagerCloseUsesExistingEntryDeadline"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"holdRead", false}}},
      {"act", "observeManagerCloseDeadline"},
      {"expect",
       {{"finiteDeadline", true},
        {"deadlinePreserved", true},
        {"explicitReason", true},
        {"stoppedAtInvocation", false},
        {"closeSucceeded", true},
        {"authority", "Missing"},
        {"onClosingCalls", 1}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ManagerClosePreservesDeadlineAcrossHeldRead)
{
    run_and_check (nlohmann::json{
      {"name", "ManagerClosePreservesDeadlineAcrossHeldRead"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"holdRead", true}}},
      {"act", "observeManagerCloseDeadline"},
      {"expect",
       {{"finiteDeadline", true},
        {"deadlinePreserved", true},
        {"explicitReason", true},
        {"stoppedAtInvocation", false},
        {"closeSucceeded", true},
        {"authority", "Missing"},
        {"onClosingCalls", 1},
        {"entryReturnedWhileReadHeld", true},
        {"callbackBeforeReadRelease", false}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, CommandDeadlineUtcConversionPreservesRange)
{
    using clock_t = std::chrono::system_clock;
    using zf::runtime::messaging::request_deadline_utc_from_unix_ms;
    constexpr std::uint64_t wire_ms = 1793310579123;
    EXPECT_EQ (request_deadline_utc_from_unix_ms (wire_ms),
               clock_t::time_point{std::chrono::milliseconds{wire_ms}});
    EXPECT_EQ (request_deadline_utc_from_unix_ms (0), clock_t::time_point{});
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                          clock_t::time_point::max ().time_since_epoch ())
                          .count ();
    EXPECT_EQ (request_deadline_utc_from_unix_ms (static_cast<std::uint64_t> (max_ms)),
               clock_t::time_point{std::chrono::milliseconds{max_ms}});
    EXPECT_EQ (request_deadline_utc_from_unix_ms (static_cast<std::uint64_t> (max_ms) + 1),
               clock_t::time_point::max ());
    EXPECT_EQ (request_deadline_utc_from_unix_ms (std::numeric_limits<std::uint64_t>::max ()),
               clock_t::time_point::max ());
}

TEST (ZLinkFrameworkSpotCloseConformance, RemoteClosingPreservesDefaultOperationDeadline)
{
    run_and_check (nlohmann::json{{"name", "remote-close-default-deadline"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "observeRemoteCloseDeadline"},
                                  {"expect",
                                   {{"finiteDeadline", true},
                                    {"deadlinePreserved", true},
                                    {"explicitReason", true},
                                    {"stoppedAtInvocation", false},
                                    {"authorityDuringClose", "Closing"},
                                    {"closeSucceeded", true},
                                    {"authority", "Missing"},
                                    {"secondClose", false},
                                    {"onClosingCalls", 1},
                                    {"authorityReleaseCalls", 1},
                                    {"terminalReplayPreserved", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, RemoteClosingPreservesExactCommandDeadline)
{
    run_and_check (nlohmann::json{
      {"name", "remote-close-exact-command-deadline"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"nativeCommand", true}}},
      {"act", "observeRemoteCloseDeadline"},
      {"expect",
       {{"finiteDeadline", true},
        {"deadlinePreserved", true},
        {"explicitReason", true},
        {"stoppedAtInvocation", false},
        {"authorityDuringClose", "Closing"},
        {"closeSucceeded", true},
        {"authority", "Missing"},
        {"secondClose", false},
        {"onClosingCalls", 1},
        {"authorityReleaseCalls", 1},
        {"terminalReplayPreserved", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_join_submit)
{
    run_and_check (nlohmann::json{{"name", "self-close-join_submit"},
                                  {"given", {{"phase", "join"}, {"await", false}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_join_await)
{
    run_and_check (nlohmann::json{{"name", "self-close-join_await"},
                                  {"given", {{"phase", "join"}, {"await", true}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_joined_submit)
{
    run_and_check (nlohmann::json{{"name", "self-close-joined_submit"},
                                  {"given", {{"phase", "joined"}, {"await", false}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_joined_await)
{
    run_and_check (nlohmann::json{{"name", "self-close-joined_await"},
                                  {"given", {{"phase", "joined"}, {"await", true}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_leave_submit)
{
    run_and_check (nlohmann::json{{"name", "self-close-leave_submit"},
                                  {"given", {{"phase", "leave"}, {"await", false}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, SelfClose_leave_await)
{
    run_and_check (nlohmann::json{{"name", "self-close-leave_await"},
                                  {"given", {{"phase", "leave"}, {"await", true}}},
                                  {"act", "closeAfterLifecycleCallback"},
                                  {"expect", {{"checks", 1}, {"closed", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ExistingThenClosingRequestAndJoinReturnNotFound)
{
    run_and_check (nlohmann::json{{"name", "existing-then-closing-admission"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "existingThenClosingAdmission"},
                                  {"expect",
                                   {{"authorityDuringClose", "Closing"},
                                    {"requestResult", "NotFound"},
                                    {"joinResult", "NotFound"},
                                    {"closeSucceeded", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ConcurrentSameRefCloseJoinsDuringOnClosing)
{
    run_and_check (nlohmann::json{{"name", "same-ref-close-on-closing"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "sameRefCloseJoins"},
                                  {"expect",
                                   {{"authorityDuringClose", "Closing"},
                                    {"joinedBeforeRelease", true},
                                    {"firstCompletedBeforeRelease", false},
                                    {"secondCompletedBeforeRelease", false},
                                    {"firstResult", true},
                                    {"secondResult", true},
                                    {"sameResult", true},
                                    {"thirdResult", false},
                                    {"onClosingCalls", 1}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ConcurrentSameRefCloseJoinsDuringAuthorityRelease)
{
    run_and_check (nlohmann::json{{"name", "same-ref-close-authority-release"},
                                  {"given", {{"authority", "Ready"}, {"holdDelete", true}}},
                                  {"act", "sameRefCloseJoins"},
                                  {"expect",
                                   {{"authorityDuringClose", "Closing"},
                                    {"joinedBeforeRelease", true},
                                    {"firstCompletedBeforeRelease", false},
                                    {"secondCompletedBeforeRelease", false},
                                    {"firstResult", true},
                                    {"secondResult", true},
                                    {"sameResult", true},
                                    {"thirdResult", false},
                                    {"onClosingCalls", 1}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, OtherGenerationCloseDuringClosingIsInvalidOperation)
{
    run_and_check (nlohmann::json{
      {"name", "other-generation-during-closing"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"otherGeneration", true}}},
      {"act", "sameRefCloseJoins"},
      {"expect",
       {{"authorityDuringClose", "Closing"},
        {"otherCompletedBeforeRelease", true},
        {"firstResult", true},
        {"otherResult", "InvalidOperation"},
        {"onClosingCalls", 1}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, RemoteJoinQueuedBehindCloseReturnsUnavailable)
{
    run_and_check (nlohmann::json{{"name", "remote-join-queued-behind-close"},
                                  {"given", {{"authority", "Ready"}, {"remote", true}}},
                                  {"act", "joinQueuedBehindClose"},
                                  {"expect",
                                   {{"closePendingBeforeRelease", true},
                                    {"joinPendingBeforeRelease", true},
                                    {"closeResult", true},
                                    {"removedBeforeJoin", true},
                                    {"joinResult", "Unavailable"},
                                    {"joinsAdmitted", 0}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, LocalJoinQueuedBehindCloseReturnsNotFound)
{
    run_and_check (nlohmann::json{{"name", "local-join-queued-behind-close"},
                                  {"given", {{"authority", "Ready"}, {"remote", false}}},
                                  {"act", "joinQueuedBehindClose"},
                                  {"expect",
                                   {{"closePendingBeforeRelease", true},
                                    {"joinPendingBeforeRelease", true},
                                    {"closeResult", true},
                                    {"removedBeforeJoin", true},
                                    {"joinResult", "NotFound"},
                                    {"joinsAdmitted", 0}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, GetOrCreateWaitsForClosing)
{
    run_and_check (nlohmann::json{{"name", "get-or-create-closing"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "getOrCreateDuringClose"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"closeSucceeded", true},
                                    {"created", true},
                                    {"newIncarnation", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, GetOrCreateClosingDeadline)
{
    run_and_check (nlohmann::json{
      {"name", "get-or-create-closing-deadline"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"deadlineExpires", true}}},
      {"act", "getOrCreateDuringClose"},
      {"expect", {{"deadlineExceeded", true}, {"closeSucceeded", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, PreCancelledCloseReportsOneClosingFailure)
{
    run_and_check (
      nlohmann::json{{"name", "pre-cancelled-close"},
                     {"given", {{"authority", "Ready"}}},
                     {"act", "preCancelledClose"},
                     {"expect",
                      {{"closeSucceeded", true},
                       {"onClosingCalls", 0},
                       {"diagnostics", nlohmann::json::array ({"onClosingFailed"})}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, EntryShutdownPreservesDeadlineAndCleanupToken)
{
    run_and_check (nlohmann::json{{"name", "entry-shutdown-context"},
                                  {"given", {{"authority", "Ready"}}},
                                  {"act", "shutdownEntryClosing"},
                                  {"expect",
                                   {{"entryDeadlinePreserved", true},
                                    {"entryShutdownReason", true},
                                    {"entrySharesCleanupToken", true},
                                    {"entryStoppedAtInvocation", false},
                                    {"entryNativeNodesAtInvocation", 1},
                                    {"terminalObserved", true},
                                    {"deadlineExceeded", false}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, EntryShutdownDeadlineCancelsCleanup)
{
    run_and_check (nlohmann::json{{"name", "entry-shutdown-cancellation"},
                                  {"given", {{"authority", "Ready"}, {"entryBlocks", true}}},
                                  {"act", "shutdownEntryClosing"},
                                  {"expect",
                                   {{"entryDeadlinePreserved", true},
                                    {"entryShutdownReason", true},
                                    {"entrySharesCleanupToken", true},
                                    {"entryStoppedAtInvocation", false},
                                    {"entryNativeNodesAtInvocation", 1},
                                    {"entryCleanupStopped", true},
                                    {"entrySawCancellation", true},
                                    {"terminalObserved", true},
                                    {"deadlineExceeded", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownPreservesClosingDeadline)
{
    run_and_check (nlohmann::json{{"name", "shutdown-closing-context"},
                                  {"given", {{"authority", "Ready"}}},
                                  {"act", "shutdownOnClosing"},
                                  {"expect",
                                   {{"deadlinePreserved", true},
                                    {"shutdownReason", true},
                                    {"stoppedAtInvocation", false},
                                    {"terminalObserved", true},
                                    {"deadlineExceeded", false}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownDeadlineCancelsClosingCleanup)
{
    run_and_check (nlohmann::json{{"name", "shutdown-closing-cancellation"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "shutdownOnClosing"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"deadlinePreserved", true},
                                    {"shutdownReason", true},
                                    {"stoppedAtInvocation", false},
                                    {"cleanupStopped", true},
                                    {"callbackSawCancellation", true},
                                    {"contextAliveAtCancellation", true},
                                    {"terminalObserved", true},
                                    {"deadlineExceeded", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownJoinsExplicitCloseBeforeOwnerCleanup)
{
    run_and_check (nlohmann::json{{"name", "shutdown-joins-close"},
                                  {"given", {{"authority", "Ready"}}},
                                  {"act", "shutdownDuringClose"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"authorityBeforeRelease", "Closing"},
                                    {"ownerDeletedBeforeRelease", false},
                                    {"closeSucceeded", true},
                                    {"deadlineExceeded", false},
                                    {"stopped", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownCloseJoinUsesDrainDeadline)
{
    run_and_check (nlohmann::json{{"name", "shutdown-close-deadline"},
                                  {"given", {{"authority", "Ready"}, {"deadlineExpires", true}}},
                                  {"act", "shutdownDuringClose"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"authorityBeforeRelease", "Ready"},
                                    {"ownerDeletedBeforeRelease", false},
                                    {"closeSucceeded", true},
                                    {"deadlineExceeded", true},
                                    {"descriptorsAfterShutdown", 1},
                                    {"stopped", false}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, RunsEveryFixtureScenario)
{
    const auto &fixture = spot_close_fixture ();
    ASSERT_EQ ("zlink.framework.spot-close", fixture.at ("fixture").get<std::string> ());
    ASSERT_EQ (1, fixture.at ("version").get<int> ());
    const auto &invariants = fixture.at ("invariants");
    EXPECT_TRUE (invariants.at ("contextCloseReturnsValue").get<bool> ());
    static_assert (
      std::is_same_v<decltype (std::declval<zf::instance_spot_context_t &> ().close ()),
                     zf::task_t<bool>>);

    for (const auto &scenario : fixture.at ("scenarios")) {
        const auto name = scenario.at ("name").get<std::string> ();
        ASSERT_TRUE (known_scenarios ().contains (name)) << "unknown scenario " << name;
        run_and_check (scenario);
    }
}

// Closing requests without activation intent never execute the old handler.
TEST (ZLinkFrameworkSpotCloseConformance, ClosingWithoutIntentReturnsNotFound)
{
    const auto &branches = spot_close_fixture ().at ("closeBranches");
    const auto branch = std::find_if (branches.begin (), branches.end (), [] (const auto &item) {
        return item.at ("name") == "closing-message-without-intent";
    });
    ASSERT_NE (branch, branches.end ());
    ASSERT_FALSE (branch->at ("given").at ("messageIntent").get<bool> ());
    ASSERT_EQ ("Closing", branch->at ("given").at ("authority"));
    ASSERT_EQ (0, branch->at ("expect").at ("newHandlerCalls"));
    auto scenario = nlohmann::json{
      {"name", branch->at ("name")},
      {"given", {{"runtime", "Ready"}, {"authority", "Ready"}, {"onClosing", "blocks"}}},
      {"act", "directRequestDuringClose"},
      {"expect",
       {{"authorityDuringClose", "Closing"},
        {"result", branch->at ("expect").at ("messageTerminal")},
        {"handlerCalls", branch->at ("expect").at ("oldHandlerCalls")},
        {"factoryCalls", branch->at ("expect").at ("factoryCalls")}}}};
    run_and_check (scenario);
}

TEST (ZLinkFrameworkSpotCloseConformance, NoPendingIntentReleasesAuthority)
{
    const auto &branches = spot_close_fixture ().at ("closeBranches");
    const auto branch = std::find_if (branches.begin (), branches.end (), [] (const auto &item) {
        return item.at ("name") == "release-without-pending-intent";
    });
    ASSERT_NE (branch, branches.end ());
    ASSERT_EQ ("Serving", branch->at ("given").at ("host"));
    ASSERT_FALSE (branch->at ("given").at ("pendingIntent").get<bool> ());
    ASSERT_EQ (0, branch->at ("expect").at ("newHandlerCalls"));
    run_and_check (nlohmann::json{{"name", branch->at ("name")},
                                  {"given", {{"runtime", "Ready"}, {"authority", "Ready"}}},
                                  {"act", "managerClose"},
                                  {"expect",
                                   {{"result", true},
                                    {"authority", branch->at ("expect").at ("authority")},
                                    {"handlerCalls", branch->at ("expect").at ("oldHandlerCalls")},
                                    {"factoryCalls", branch->at ("expect").at ("factoryCalls")},
                                    {"onClosingCalls", 1},
                                    {"order", branch->at ("expect").at ("order")}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, CloseCancelsTimerWithoutExecutingAfterOnClosing)
{
    const auto &fixture = spot_close_fixture ();
    ASSERT_TRUE (fixture.at ("invariants").at ("unexecutedTimersCancelled").get<bool> ());
    run_and_check (nlohmann::json{{"name", "close-cancels-timer-without-old-tick-after-on-closing"},
                                  {"given",
                                   {{"runtime", "Ready"},
                                    {"authority", "Ready"},
                                    {"roomTimer", true},
                                    {"onClosing", "blocks"}}},
                                  {"act", "directRequestDuringClose"},
                                  {"expect",
                                   {{"result", "NotFound"},
                                    {"authority", "Missing"},
                                    {"timerDisposedAfterClose", true},
                                    {"timerCallsAfterOnClosing", 0},
                                    {"onClosingCalls", 1}}}});
}

// The Store write entry controls the CAS boundary; admission evidence controls
// the queued request boundary without a test sleep.
TEST (ZLinkFrameworkSpotCloseConformance, QueuedRequestWaitsForClosingCommit)
{
    const auto scenarios = nlohmann::json::parse (R"json([
      {"name": "queued-request-after-closing-conflict-executes",
       "given": {"runtime": "Ready", "authority": "Ready", "closingCommit": "heldThenConflict"},
       "act": "directRequestDuringHeldClosingCommit",
       "expect": {"handlerCallsBeforeCommit": 1, "result": "handlerReply", "closeResult": "Unavailable",
                  "authority": "Ready", "admission": "open"}},
      {"name": "queued-request-after-closing-success-is-not-found",
       "given": {"runtime": "Ready", "authority": "Ready", "closingCommit": "held"},
       "act": "directRequestDuringHeldClosingCommit",
       "expect": {"handlerCallsBeforeCommit": 1, "result": "NotFound", "closeResult": true,
                  "authority": "Missing", "handlerCalls": 1}}
    ])json");
    for (const auto &scenario : scenarios)
        run_and_check (scenario);
}

// Two Actor Joins that move members between two User Spots in opposite
// directions at the same time complete (Spot-Actor membership: Join on the
// lifecycle lane; handler turn and execution gate §7).
TEST (ZLinkFrameworkSpotCloseConformance, OppositeJoinsBetweenTwoSpotsComplete)
{
    run_and_check (nlohmann::json::parse (R"json(
      {"name": "opposite-joins-complete",
       "given": {"runtime": "Ready", "authority": "Ready"},
       "act": "crossJoins",
       "expect": {"crossJoins": "completed"}})json"));
}

TEST (ZLinkFrameworkSpotCloseConformance, TimerWaitsForReadyStorePublication)
{
    for (const bool instance : {false, true}) {
        run_and_check (nlohmann::json{
          {"name", instance ? "instance-timer-ready-publication" : "user-timer-ready-publication"},
          {"given", {{"authority", "Missing"}, {"instance", instance}, {"heldStage", "ready"}}},
          {"act", "timerDuringPreparedAuthorityWrite"},
          {"expect",
           {{"timerCallsWhileHeld", 0},
            {"handlerCallsWhileHeld", 0},
            {"timerCallsAfterPublication", 1},
            {"created", true}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance, InstanceReadyTimerRunsWhileDurableTerminalDefersClose)
{
    for (const auto *stage : {"cursor", "pointer"}) {
        run_and_check (
          nlohmann::json{{"name", std::string ("instance-timer-first-terminal-") + stage},
                         {"given",
                          {{"authority", "Missing"},
                           {"instance", true},
                           {"heldStage", stage},
                           {"timerCloses", true}}},
                         {"act", "timerDuringPreparedAuthorityWrite"},
                         {"expect",
                          {{"timerCallsWhileHeld", 1},
                           {"handlerCallsWhileHeld", 1},
                           {"timerCloseRequestedWhileHeld", true},
                           {"timerCloseCompletedWhileHeld", false},
                           {"timerCloseResult", true},
                           {"timerCallsAfterPublication", 1},
                           {"created", true}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance,
      InstanceDurableTerminalFailureDiscardsPreparationAndCloseIsFalse)
{
    for (const auto *stage : {"cursor", "pointer"}) {
        run_and_check (
          nlohmann::json{{"name", std::string ("instance-timer-terminal-failure-") + stage},
                         {"given",
                          {{"authority", "Missing"},
                           {"instance", true},
                           {"heldStage", stage},
                           {"timerCloses", true},
                           {"terminalConflict", true}}},
                         {"act", "timerDuringPreparedAuthorityWrite"},
                         {"expect",
                          {{"timerCallsWhileHeld", 1},
                           {"handlerCallsWhileHeld", 1},
                           {"timerCloseRequestedWhileHeld", true},
                           {"timerCloseCompletedWhileHeld", false},
                           {"timerCloseResult", false},
                           {"created", false},
                           {"onClosingCalls", 0}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance, TimerCloseSucceedsAfterReadyPublication)
{
    run_and_check (nlohmann::json{
      {"name", "timer-close-after-ready-publication"},
      {"given", {{"authority", "Missing"}, {"heldStage", "ready"}, {"timerCloses", true}}},
      {"act", "timerDuringPreparedAuthorityWrite"},
      {"expect",
       {{"timerCallsWhileHeld", 0},
        {"timerCallsAfterPublication", 1},
        {"timerCloseResult", true},
        {"created", true}}}});
}
} // namespace
