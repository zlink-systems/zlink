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
#include "spot_close_scenario_application.hpp"
#include "spot_close_scenario_fixture.hpp"
#include "spot_close_preparation_fixture.hpp"

#ifndef ZLINK_SPOT_CLOSE_CONFORMANCE_PATH
#error "Spot Close conformance fixture path is required"
#endif

namespace zlink::framework::test::spot_close
{
void observe_scheduled_expiry (
  const std::shared_ptr<zlink::framework::runtime::offload_executor_t> &executor)
{
    auto marker = std::make_shared<std::promise<void>> ();
    auto observed = marker->get_future ();
    if (!executor->try_submit_internal ([marker] { marker->set_value (); }))
        throw std::runtime_error ("expiry observation worker rejected the marker");
    observed.get ();
}

observation_t *observed = nullptr;

observation_t &current ()
{
    return *observed;
}

zf::task_t<void> member_actor_t::on_join_completed (const zf::actor_join_completion_t &completion)
{
    std::string note = "actorJoin:";
    if (std::holds_alternative<zf::actor_join_accepted_t> (completion))
        note += "accepted";
    else if (std::holds_alternative<zf::actor_join_rejected_t> (completion))
        note += "rejected";
    else
        note += "failed:"
                + std::to_string (
                  static_cast<int> (std::get<zf::actor_join_failed_t> (completion).error_kind));
    std::lock_guard lock (current ().mutex);
    current ().notes.push_back (std::move (note));
    current ().cross_progress.notify_all ();
    const auto id = _context.actor_id ().value ();
    if (id == "closing-admission-member")
        current ().closing_join_completed.set_value (completion);
    if (current ().cross_join_active.load ()) {
        current ().order.push_back ("joinReply:" + std::string (id));
        current ().cross_progress.notify_all ();
    } else if (id == "cross-a" || id == "cross-b") {
        (id == "cross-a" ? current ().cross_a_initial : current ().cross_b_initial).set_value ();
    }
    co_return;
}

void install_preparation_timer (const std::shared_ptr<zf::detail::spot_context_state_t> &state)
{
    if (!current ().prepare_timer)
        return;
    auto timer = std::make_shared<zf::detail::timer_state_t> ();
    timer->name = "prepared-expiry";
    timer->period = std::chrono::hours (1);
    timer->handler_instance = state->spot_instance;
    timer->handler_invoker = [weak_state = std::weak_ptr (state)] (void *, void *,
                                                                   zf::serializer_registry_t &,
                                                                   const zf::timer_tick_t &) {
        bool closes = false;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().timer_calls;
            closes = current ().preparation_timer_closes;
        }
        if (closes) {
            auto closing = zf::detail::spot_context_access_t::create (weak_state.lock ()).close ();
            std::lock_guard lock (current ().mutex);
            current ().preparation_timer_close.emplace (std::move (closing));
        }
        return zf::task_t<zlink::message_t> (
          zf::result_t<zlink::message_t>::success (zlink::message_t{}));
    };
    if (state->execution_mode == zf::user_spot_execution_mode_t::per_actor)
        timer->serial_queue = state->spot_serial_executor->timer_queue (timer->name);
    state->node->lane.run ([&] { state->timers.push_back (timer); }).get ();
    std::lock_guard lock (current ().mutex);
    current ().preparation_timer = std::move (timer);
    current ().initialized = true;
}

// ---------------------------------------------------------------------------
// Scenario runner.

std::string error_name (zf::framework_error_kind_t kind)
{
    switch (kind) {
        case zf::framework_error_kind_t::not_found:
            return "NotFound";
        case zf::framework_error_kind_t::rejected:
            return "Rejected";
        case zf::framework_error_kind_t::unavailable:
            return "Unavailable";
        case zf::framework_error_kind_t::shutting_down:
            return "ShuttingDown";
        case zf::framework_error_kind_t::invalid_operation:
            return "InvalidOperation";
        default:
            return "Error" + std::to_string (static_cast<int> (kind));
    }
}

zf::task_t<void>
join_queued_behind_close_task (nlohmann::json *actual,
                               std::shared_ptr<zf::detail::spot_node_builder_state_t> node,
                               std::string spot_id,
                               bool remote,
                               zf::detail::spot_node_runtime_t runtime,
                               zf::actor_ref_t actor_ref,
                               zf::authority_snapshot_t authority)
{
    (*actual)["removedBeforeJoin"] =
      node->lane.run ([&] { return !node->spot_contexts_by_id.contains (spot_id); }).get ();
    if (remote) {
        const auto result = co_await zf::runtime::await_result (runtime.admit_remote_actor_to_spot (
          "queued-close-transfer", actor_ref, zf::spot_id_t ("source"), zf::spot_id_t (spot_id), {},
          0, 0, authority.authority_owner_generation,
          authority.allocation.target.node_lifecycle_generation,
          static_cast<std::uint64_t> (authority.owner.lease_generation)));
        (*actual)["joinResult"] = result ? "Accepted" : error_name (result.error_kind ());
    } else {
        const auto result = co_await zf::runtime::await_result (
          runtime.join_actor_to_spot_erased (actor_ref, zf::spot_id_t (spot_id), {}));
        (*actual)["joinResult"] = result ? "Accepted" : error_name (result.error_kind ());
    }
}

template <typename T> nlohmann::json close_result (const zf::result_t<T> &result)
{
    if (result)
        return result.value ();
    return error_name (result.error_kind ());
}

bool wait_until (const std::function<bool ()> &condition,
                 std::chrono::milliseconds limit = std::chrono::milliseconds (3000))
{
    const auto deadline = std::chrono::steady_clock::now () + limit;
    while (!condition ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::sleep_for (1ms);
    }
    return true;
}

using zlink::framework::test::wait_for_serving;

class scenario_client_t final : public zf::hosted_service_t
{
  public:
    scenario_client_t (zf::app_t &app, const nlohmann::json &scenario) :
        _app (&app), _scenario (scenario)
    {
    }

    zf::task_t<void> start (zf::service_provider_t &services) override
    {
        _worker = std::thread ([this, services] () mutable {
            wait_for_serving (services);
            execute (services);
        });
        co_return;
    }

    void join ()
    {
        if (_worker.joinable ())
            _worker.join ();
    }

    void execute (zf::service_provider_t &services)
    {
        try {
            run (services);
            // Explicit Close completed before its diagnostic snapshot and app.stop().
            // Shutdown scenarios already own their separate terminal/cleanup assertions.
            if (!stopping)
                zf::test::acknowledge_delivery (std::chrono::steady_clock::now () + 3s);
        }
        catch (const std::exception &error) {
            failure = error.what ();
        }
        {
            // Observed before the app stops: shutdown cleanup is not part of the act.
            std::lock_guard lock (current ().mutex);
            actual["onClosingCalls"] = current ().on_closing_calls;
            actual["handlerCalls"] = current ().handler_calls;
            actual["factoryCalls"] = current ().factory_calls;
            if (current ().timer_resource) {
                actual["timerDisposedAfterClose"] = current ().timer_resource->is_disposed ();
                actual["timerCallsAfterOnClosing"] =
                  current ().timer_calls - current ().timer_calls_on_closing;
            }
            actual["order"] = current ().order;
            actual["diagnostics"] = current ().diagnostics;
            actual["notes"] = current ().notes;
            actual["joinsAdmitted"] = current ().joins_admitted;
        }
        if (!stopping)
            _app->stop ();
    }

    void stop () noexcept override {}

    nlohmann::json actual = nlohmann::json::object ();
    std::string failure;
    bool stopping = false;

  private:
    std::thread _worker;
    std::string authority_state (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &locations = services.get_required<zf::location_repository_t> ();
        const auto read =
          locations.read_authority (zf::runtime::spot_authority_key (spot_id)).result ().value ();
        const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&read);
        if (!snapshot)
            return "Missing";
        if (const auto user = zf::runtime::decode_direct_user_spot_authority_payload (
              std::span<const std::byte> (snapshot->payload))) {
            switch (user->state) {
                case zf::runtime::user_spot_authority_state_t::creating:
                    return "Creating";
                case zf::runtime::user_spot_authority_state_t::ready:
                    return "Ready";
                case zf::runtime::user_spot_authority_state_t::closing:
                    return "Closing";
            }
        }
        const std::string_view instance_closing = "zlink:instance-spot:closing:v1\n";
        if (snapshot->payload.size () >= instance_closing.size ()
            && std::equal (instance_closing.begin (), instance_closing.end (),
                           snapshot->payload.begin (), [] (char left, std::byte right) {
                               return static_cast<unsigned char> (left)
                                      == std::to_integer<unsigned char> (right);
                           }))
            return "Closing";
        return "Ready";
    }

    zf::spot_ref_t create_room (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        const auto created =
          manager.get_or_create (zf::spot_id_t (spot_id), "room").timeout (3s).async ().result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "User Spot create failed");
        const auto found = manager.find (zf::spot_id_t (spot_id)).result ();
        if (!found || !found.value ())
            throw std::runtime_error ("User Spot find failed");
        return *found.value ();
    }

    zf::result_t<bool> manager_close (zf::service_provider_t &services, const zf::spot_ref_t &ref)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        return manager.close (ref).result ();
    }

    zf::result_t<close_probe_reply_t>
    direct_request (const std::string &spot_id, zf::service_provider_t &services, int value)
    {
        auto route = _app->advanced ().zlink ().route_client (
          services.get_required<zf::serializer_registry_t> ());
        return route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{value})
          .timeout (3s)
          .async<close_probe_reply_t> ()
          .result ();
    }

    void join_member (zf::service_provider_t &services,
                      const std::string &room,
                      const std::string &actor_id)
    {
        auto &actors = services.get_required<zf::actor_manager_t> ();
        const auto created = actors.get_or_create (zf::actor_id_t (actor_id), "member")
                               .timeout (3s)
                               .async ()
                               .result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "member Actor create failed");
        auto &client = services.get_required<zf::actor_client_t> ();
        const auto sent =
          client.send (zf::actor_id_t (actor_id), join_room_t{room}).async ().result ();
        if (!sent)
            throw std::runtime_error (sent.error () ? sent.error ()->what ()
                                                    : "member Join request failed");
    }

    void run (zf::service_provider_t &services)
    {
        const auto name = _scenario.at ("name").get<std::string> ();
        const auto &given = _scenario.at ("given");
        const auto act = _scenario.at ("act").get<std::string> ();
        const auto spot_id = "close-" + name;
        {
            std::lock_guard lock (current ().mutex);
            current ().watched_spot = spot_id;
            current ().on_closing_throws = given.value ("onClosing", "") == "throws";
            current ().on_closing_blocks = given.value ("onClosing", "") == "blocks";
            current ().handler_mode = given.value ("handler", "");
            current ().room_timer = given.value ("roomTimer", false);
            current ().prepare_timer = act == "timerDuringPreparedAuthorityWrite";
            current ().preparation_timer_closes = given.value ("timerCloses", false);
        }
        auto &store = *_store;

        if (act == "closeAfterLifecycleCallback") {
            const auto other_ref = create_room (services, spot_id + "-other");
            auto other = zf::detail::spot_context_access_t::create (current ().room_state);
            const auto source = create_room (services, spot_id);
            const auto target_id = spot_id + "-target";
            (void) create_room (services, target_id);
            const auto phase = given.at ("phase").get<std::string> ();
            const auto await_close = given.at ("await").get<bool> ();
            std::promise<void> checked;
            auto check_completed = checked.get_future ();
            int checks = 0;
            current ().lifecycle_close_probe =
              [&] (zf::spot_context_t &context,
                   std::string_view current_phase) -> zf::task_t<void> {
                if (context.spot_id () == spot_id && current_phase == phase) {
                    bool rejected = false;
                    try {
                        auto pending = context.close ();
                        if (await_close)
                            (void) co_await pending;
                    }
                    catch (const zf::framework_exception_t &error) {
                        EXPECT_EQ (zf::framework_error_kind_t::invalid_operation, error.kind ());
                        rejected = true;
                    }
                    EXPECT_TRUE (rejected);
                    // The non-awaiting call must throw before returning its task too.
                    EXPECT_THROW ((void) context.close (), zf::framework_exception_t);
                    EXPECT_TRUE (co_await other.close ());
                    ++checks;
                    checked.set_value ();
                }
                co_return;
            };
            auto wait_join = [&] (std::size_t count) {
                std::unique_lock lock (current ().mutex);
                current ().cross_progress.wait (lock,
                                                [&] { return current ().notes.size () >= count; });
                if (current ().notes[count - 1] != "actorJoin:accepted")
                    throw std::runtime_error ("Actor Join did not establish the test membership: "
                                              + current ().notes[count - 1]);
            };
            join_member (services, spot_id, "lifecycle-close-member");
            wait_join (1);
            join_member (services, target_id, "lifecycle-close-member");
            wait_join (2);
            check_completed.get ();
            actual["checks"] = checks;
            const auto closed = manager_close (services, source);
            actual["closed"] = closed && closed.value ();
            current ().lifecycle_close_probe = {};
            return;
        }

        if (act == "timerDuringPreparedAuthorityWrite") {
            const bool instance = given.value ("instance", false);
            const auto stage = given.at ("heldStage").get<std::string> ();
            store.hold_prepared_authority_put = stage == "ready" ? 1 : stage == "cursor" ? 2 : 3;
            auto held = store.commit_held.get_future ();
            auto creating = std::async (std::launch::async, [&] {
                if (!instance) {
                    auto &manager = services.get_required<zf::spot_manager_t> ();
                    const auto result = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                          .timeout (3s)
                                          .async ()
                                          .result ();
                    return bool (result);
                }
                auto route = _app->advanced ().zlink ().route_client (
                  services.get_required<zf::serializer_registry_t> ());
                return bool (
                  route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{0})
                    .instance_spot ("session")
                    .timeout (3s)
                    .async<close_probe_reply_t> ()
                    .result ());
            });
            const std::shared_ptr<void> release_on_exit (
              nullptr, [&] (void *) { store.release_held_commit (false); });
            held.get ();
            const auto state = instance ? current ().session_state : current ().room_state;
            const auto timer = current ().preparation_timer;
            const auto queue = state->serial_queue;
            zf::detail::timer_runtime_t::post_fire_count (state, timer, timer->serial_queue, 1);
            observe_scheduled_expiry (state->serial_executor);
            {
                std::lock_guard lock (current ().mutex);
                actual["timerCallsWhileHeld"] = current ().timer_calls;
                actual["handlerCallsWhileHeld"] = current ().handler_calls;
                if (given.value ("timerCloses", false) && stage != "ready") {
                    actual["timerCloseRequestedWhileHeld"] =
                      bool (current ().preparation_timer_close);
                    actual["timerCloseCompletedWhileHeld"] =
                      current ().preparation_timer_close
                      && current ().preparation_timer_close->await_ready ();
                }
            }
            store.release_held_commit (given.value ("terminalConflict", false));
            actual["created"] = creating.get ();
            queue->drain ();
            if (timer->serial_queue)
                timer->serial_queue->drain ();
            std::optional<zf::task_t<bool>> timer_close;
            {
                std::lock_guard lock (current ().mutex);
                actual["timerCallsAfterPublication"] = current ().timer_calls;
                timer_close = std::move (current ().preparation_timer_close);
            }
            if (timer_close)
                actual["timerCloseResult"] = close_result (timer_close->result ());
            return;
        }

        if (given.contains ("handler")) {
            // Instance Spot: the handler turn calls context Close itself.
            auto route = _app->advanced ().zlink ().route_client (
              services.get_required<zf::serializer_registry_t> ());
            const auto activated =
              route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{0})
                .instance_spot ("session")
                .timeout (3s)
                .async<close_probe_reply_t> ()
                .result ();
            if (!activated)
                throw std::runtime_error ("Instance Spot activation failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
            }
            if (act != "directRequestWithoutInstanceIntent")
                throw std::runtime_error ("unsupported act for a handler scenario: " + act);
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? "handlerReply" : "handlerFailure";
            (void) wait_until ([&] { return authority_state (services, spot_id) == "Missing"; });
            actual["authority"] = authority_state (services, spot_id);
            return;
        }

        const auto authority = given.at ("authority").get<std::string> ();
        std::optional<zf::spot_ref_t> ref;
        if (authority != "Missing")
            ref = create_room (services, spot_id);
        if (act == "observeManagerCloseDeadline") {
            auto entered = current ().closing_entered.get_future ();
            auto held = store.read_held.get_future ();
            std::promise<void> submitted;
            auto submission = submitted.get_future ();
            const bool hold_read = given.value ("holdRead", false);
            store.hold_read_once.store (hold_read);
            std::chrono::system_clock::time_point before, after;
            std::chrono::steady_clock::time_point steady_before, steady_after;
            auto closing = std::async (std::launch::async, [&] {
                steady_before = std::chrono::steady_clock::now ();
                before = std::chrono::system_clock::now ();
                auto pending = services.get_required<zf::spot_manager_t> ().close (*ref);
                after = std::chrono::system_clock::now ();
                steady_after = std::chrono::steady_clock::now ();
                submitted.set_value ();
                return pending.result ();
            });
            // Release every fixture barrier before the async future can join on an exception.
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                store.release_held_read ();
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (submission.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("public Close did not return its task");
            if (hold_read) {
                if (held.wait_for (3s) != std::future_status::ready)
                    throw std::runtime_error ("source Close did not reach Store read");
                actual["callbackBeforeReadRelease"] =
                  entered.wait_for (0ms) == std::future_status::ready;
                actual["entryReturnedWhileReadHeld"] = true;
                // Controlled Store latency exceeds clock conversion precision; no caller expiry
                // or callback ordering is inferred from this interval.
                std::mutex hold_mutex;
                std::condition_variable hold_duration;
                std::unique_lock hold_lock (hold_mutex);
                hold_duration.wait_until (hold_lock, steady_after + 25ms, [] { return false; });
                actual["readReleasedSteadyNs"] =
                  std::chrono::steady_clock::now ().time_since_epoch ().count ();
                store.release_held_read ();
            }
            if (entered.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("Close did not invoke OnClosing");
            {
                std::lock_guard lock (current ().mutex);
                const auto context = current ().closing_context;
                actual["finiteDeadline"] =
                  context && context->deadline != std::chrono::system_clock::time_point::max ();
                const auto callback_ms = context
                                           ? std::chrono::duration_cast<std::chrono::milliseconds> (
                                               context->deadline.time_since_epoch ())
                                               .count ()
                                           : 0;
                const auto callback_at = zf::runtime::messaging::request_deadline_from_unix_ms (
                  static_cast<std::uint64_t> (callback_ms));
                actual["deadlinePreserved"] = context && callback_at >= steady_before + 30s - 1ms
                                              && callback_at <= steady_after + 30s + 1ms;
                actual["callbackSteadyNs"] = callback_at.time_since_epoch ().count ();
                actual["entryLowerSteadyNs"] = steady_before.time_since_epoch ().count ();
                actual["entryUpperSteadyNs"] = steady_after.time_since_epoch ().count ();
                actual["explicitReason"] =
                  context && context->reason == zf::spot_close_reason_t::explicit_close;
                actual["stoppedAtInvocation"] = current ().cleanup_stopped_at_invocation;
                actual["callbackDeadlineMs"] =
                  context ? std::chrono::duration_cast<std::chrono::milliseconds> (
                              context->deadline.time_since_epoch ())
                              .count ()
                          : 0;
                actual["entryLowerMs"] =
                  std::chrono::duration_cast<std::chrono::milliseconds> (before.time_since_epoch ())
                    .count ();
                actual["entryUpperMs"] =
                  std::chrono::duration_cast<std::chrono::milliseconds> (after.time_since_epoch ())
                    .count ();
            }
            current ().closing_released.complete (zf::result_t<void>::success ());
            const auto closed = closing.get ();
            actual["closeSucceeded"] = closed && closed.value ();
            actual["authority"] = authority_state (services, spot_id);
            return;
        }
        if (act == "observeRemoteCloseDeadline") {
            const auto entered = current ().closing_entered.get_future ();
            const auto before = std::chrono::system_clock::now ();
            const bool native = given.value ("nativeCommand", false);
            const auto wire_ms =
              static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                            before.time_since_epoch () + 57s + 123ms)
                                            .count ());
            std::atomic_bool replay_preserved{false};
            auto closing = std::async (std::launch::async, [&] {
                if (!native)
                    return manager_close (services, *ref);
                auto runtime =
                  zf::detail::spot_node_runtime_t::from (_app->advanced ().zlink (), mesh_name);
                const auto node = runtime->native_node ();
                if (!node)
                    throw std::runtime_error ("Spot owner has no native node");
                auto &host = *node;
                const auto status = host.status ();
                const auto read = services.get_required<zf::location_repository_t> ()
                                    .read_authority (zf::runtime::spot_authority_key (spot_id))
                                    .result ()
                                    .value ();
                const auto &snapshot = std::get<zf::authority_snapshot_t> (read);
                zf::runtime::protocol::user_spot_close_header_t command{
                  0,
                  {status.lifecycle_generation (), 7139},
                  status.routing_id ().to_bytes (),
                  status.lifecycle_generation (),
                  {ref->spot_id (), ref->object_generation (), status.routing_id ().to_bytes (),
                   status.lifecycle_generation (), snapshot.authority_owner_generation,
                   snapshot.store_version},
                  wire_ms};
                const auto submit = [&] (const auto &request) {
                    zf::task_completion_source_t<bool> completed;
                    auto terminal = completed.task ();
                    const auto submitted =
                      host
                        .close_user_spot_remote (
                          status.routing_id (), request, std::chrono::steady_clock::now () + 3s,
                          [&] (zf::runtime::foundation::operation_terminal_t outcome,
                               zf::runtime::protocol::user_spot_close_reply_t reply) {
                              completed.complete (zf::result_t<bool>::success (
                                outcome == zf::runtime::foundation::operation_terminal_t::completed
                                && reply.header.correlation != 0
                                && reply.header.terminal_result == 0 && reply.closed));
                          })
                        .result ();
                    if (!submitted || !submitted.value ())
                        throw std::runtime_error ("command 48 was not admitted");
                    return terminal.result ();
                };
                const auto closed = submit (command);
                // Reuse the original source generation, operation ID and deadline.
                const auto replay = submit (command);
                replay_preserved.store (replay && replay.value ());
                return closed;
            });
            // Release before the async future joins, including setup failures.
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (entered.wait_for (3s) != std::future_status::ready) {
                current ().closing_released.complete (zf::result_t<void>::success ());
                (void) closing.get ();
                throw std::runtime_error ("OnClosing did not start");
            }
            const auto after = std::chrono::system_clock::now ();
            {
                std::lock_guard lock (current ().mutex);
                const auto context = current ().closing_context;
                actual["finiteDeadline"] =
                  context && context->deadline != std::chrono::system_clock::time_point::max ();
                actual["explicitReason"] =
                  context && context->reason == zf::spot_close_reason_t::explicit_close;
                actual["stoppedAtInvocation"] = current ().cleanup_stopped_at_invocation;
                const auto callback_ms = context
                                           ? std::chrono::duration_cast<std::chrono::milliseconds> (
                                               context->deadline.time_since_epoch ())
                                               .count ()
                                           : 0;
                actual["callbackDeadlineMs"] = callback_ms;
                actual["wireDeadlineMs"] = native ? nlohmann::json (wire_ms) : nullptr;
                actual["deadlinePreserved"] =
                  native ? callback_ms >= 0 && static_cast<std::uint64_t> (callback_ms) == wire_ms
                         : context && context->deadline >= before + 30s - 1ms
                             && context->deadline
                                  <= std::chrono::ceil<std::chrono::milliseconds> (after + 30s);
            }
            actual["authorityDuringClose"] = authority_state (services, spot_id);
            current ().closing_released.complete (zf::result_t<void>::success ());
            const auto closed = closing.get ();
            actual["closeSucceeded"] = closed && closed.value ();
            actual["terminalReplayPreserved"] = !native || replay_preserved.load ();
            actual["authority"] = authority_state (services, spot_id);
            // The original ref has no incarnation left to close; callback stays once.
            actual["secondClose"] = close_result (manager_close (services, *ref));
            {
                std::lock_guard lock (current ().mutex);
                actual["authorityReleaseCalls"] = std::count (
                  current ().order.begin (), current ().order.end (), "authorityReleased");
            }
            return;
        }
        if (act == "preCancelledClose") {
            std::stop_source cancelled;
            cancelled.request_stop ();
            auto runtime =
              zf::detail::spot_node_runtime_t::from (_app->advanced ().zlink (), mesh_name);
            const auto result =
              runtime
                ->close_spot (zf::spot_id_t (spot_id), zf::spot_close_reason_t::explicit_close,
                              std::chrono::system_clock::now () + 1s, cancelled.get_token ())
                .result ();
            actual["closeSucceeded"] = result && result.value ();
            return;
        }
        if (act == "shutdownEntryClosing") {
            current ().observe_entry_closing = true;
            current ().entry_closing_blocks = given.value ("entryBlocks", false);
            current ().shutdown_runtime = &services.get_required<zf::framework_runtime_t> ();
            auto shutdown = _app->shutdown (500ms);
            stopping = true;
            const auto terminal = shutdown.result_for (3s);
            {
                std::lock_guard lock (current ().mutex);
                const auto context = current ().entry_closing_context;
                actual["entryDeadlinePreserved"] =
                  context && current ().host_closing_deadline
                  && context->deadline == *current ().host_closing_deadline;
                actual["entryShutdownReason"] =
                  context && context->reason == zf::spot_close_reason_t::host_shutdown;
                actual["entrySharesCleanupToken"] =
                  current ().cleanup_token == current ().entry_cleanup_token;
                actual["entryStoppedAtInvocation"] = current ().entry_stopped_at_invocation;
                actual["entryNativeNodesAtInvocation"] =
                  current ().entry_native_nodes_at_invocation;
                actual["entryCleanupStopped"] = current ().entry_cleanup_token.stop_requested ();
                actual["entrySawCancellation"] = current ().entry_saw_cancellation.load ();
            }
            current ().entry_closing_released.complete (zf::result_t<void>::success ());
            actual["terminalObserved"] = terminal.has_value ();
            actual["deadlineExceeded"] =
              terminal && terminal->value ().reason == zf::termination_reason_t::deadline_exceeded;
            return;
        }
        if (act == "shutdownOnClosing") {
            current ().shutdown_runtime = &services.get_required<zf::framework_runtime_t> ();
            auto shutdown = _app->shutdown (500ms);
            stopping = true;
            auto terminal = shutdown.result_for (3s);
            actual["completedBeforeRelease"] = terminal.has_value ();
            {
                std::lock_guard lock (current ().mutex);
                const auto context = current ().closing_context;
                actual["deadlinePreserved"] =
                  context && current ().host_closing_deadline
                  && context->deadline == *current ().host_closing_deadline;
                actual["shutdownReason"] =
                  context && context->reason == zf::spot_close_reason_t::host_shutdown;
                actual["stoppedAtInvocation"] = current ().cleanup_stopped_at_invocation;
                actual["cleanupStopped"] = current ().cleanup_token.stop_requested ();
                actual["callbackSawCancellation"] = current ().cleanup_seen_in_callback.load ();
                actual["contextAliveAtCancellation"] = current ().cleanup_context_preserved.load ();
            }
            current ().closing_released.complete (zf::result_t<void>::success ());
            terminal = shutdown.result ();
            actual["terminalObserved"] = terminal.has_value ();
            actual["deadlineExceeded"] =
              terminal && terminal->value ().reason == zf::termination_reason_t::deadline_exceeded;
            return;
        }
        if (act == "shutdownDuringClose") {
            // Hold the fenced release after local teardown has marked the Spot closed.
            // Shutdown must retain the authority until this existing Close settles.
            const bool expires = given.value ("deadlineExpires", false);
            if (expires)
                store.hold_closing_commit_once.store (true);
            else
                store.hold_authority_release_once.store (true);
            auto held = store.commit_held.get_future ();
            // Exercise the node's explicit Close directly: manager request accounting
            // would make the earlier host completion barrier mask this ordering bug.
            auto runtime =
              zf::detail::spot_node_runtime_t::from (_app->advanced ().zlink (), mesh_name);
            auto closing = runtime->close_spot (zf::spot_id_t (spot_id));
            if (held.wait_for (3s) != std::future_status::ready) {
                store.release_held_commit (false);
                throw std::runtime_error ("Close did not reach authority release");
            }
            auto &repository = services.get_required<zf::location_repository_t> ();
            auto shutdown = _app->shutdown (expires ? 100ms : 3s);
            stopping = true;
            if (!expires) {
                const auto state = current ().room_state;
                auto owner = state->lane_owner.lock ();
                if (!owner)
                    owner = state->node;
                if (!wait_until ([&] {
                        return store.owner_lease_released.load ()
                               || owner->lane
                                    .run ([&] { return !state->merged_close_results.empty (); })
                                    .get ();
                    })) {
                    store.release_held_commit (false);
                    throw std::runtime_error ("shutdown did not reach the Close barrier");
                }
            }
            auto terminal = shutdown.result_for (expires ? 2s : 0ms);
            actual["completedBeforeRelease"] = terminal.has_value ();
            actual["authorityBeforeRelease"] = authority_state (services, spot_id);
            actual["ownerDeletedBeforeRelease"] = store.owner_lease_released.load ();
            store.release_held_commit (false);
            const auto close_result = closing.result ();
            actual["closeSucceeded"] = close_result && close_result.value ();
            if (!terminal)
                terminal = shutdown.result_for (3s);
            if (!terminal)
                throw std::runtime_error ("shutdown exceeded its bounded teardown");
            const auto result = terminal->value ();
            actual["deadlineExceeded"] =
              result.reason == zf::termination_reason_t::deadline_exceeded;
            actual["stopped"] = result.outcome == zf::termination_outcome_t::stopped;
            actual["descriptorsAfterShutdown"] =
              repository.list_mesh_nodes (mesh_name).result ().value ().items.size ();
            return;
        }
        if (authority == "Closing") {
            // A Close whose authority release fails leaves Closing committed.
            store.fail_authority_release_once.store (true);
            (void) manager_close (services, *ref);
            if (authority_state (services, spot_id) != "Closing")
                throw std::runtime_error ("could not establish a Closing authority");
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        if (given.value ("members", 0) > 0) {
            join_member (services, spot_id, "member-" + name);
            if (!wait_until ([&] {
                    std::lock_guard lock (current ().mutex);
                    return std::count (current ().order.begin (), current ().order.end (),
                                       "joinCompleted")
                           > 0;
                }))
                throw std::runtime_error ("member Join did not complete");
        }
        if (given.value ("closingCommit", "") == "ownerFenceMismatch")
            store.fail_closing_commit_once.store (true);
        if (given.value ("failOnce", "") == "authorityReleased")
            store.fail_authority_release_once.store (true);

        if (act == "crossJoins") {
            // Two members move between two rooms in opposite directions at
            // the same time; each Join also leaves the other room.
            const auto room_a = spot_id;
            const auto room_b = spot_id + "-b";
            (void) create_room (services, room_b);
            // Membership §4.2 sends source Leave one-way before Join completion.
            // Observe both callbacks within the same deadline before checking order.
            auto initial_a = current ().cross_a_initial.get_future ();
            auto initial_b = current ().cross_b_initial.get_future ();
            join_member (services, room_a, "cross-a");
            if (initial_a.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("member cross-a did not join room A");
            join_member (services, room_b, "cross-b");
            if (initial_b.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("member cross-b did not join room B");
            auto source_a_reserved = current ().cross_a_reserved.get_future ();
            auto source_b_reserved = current ().cross_b_reserved.get_future ();
            current ().cross_join_active.store (true);
            auto &client = services.get_required<zf::actor_client_t> ();
            auto to_b = std::async (std::launch::async, [&] {
                return client.send (zf::actor_id_t ("cross-a"), join_room_t{room_b})
                  .async ()
                  .result ();
            });
            auto to_a = std::async (std::launch::async, [&] {
                return client.send (zf::actor_id_t ("cross-b"), join_room_t{room_a})
                  .async ()
                  .result ();
            });
            const auto registration_deadline = std::chrono::steady_clock::now () + 8000ms;
            const bool both_reserved =
              source_a_reserved.wait_until (registration_deadline) == std::future_status::ready
              && source_b_reserved.wait_until (registration_deadline) == std::future_status::ready;
            current ().cross_sources_released.set_value ();
            (void) to_b.get ();
            (void) to_a.get ();
            if (!both_reserved)
                throw std::runtime_error ("both source handlers must register their Join barrier");
            bool completed = false;
            bool replies_after_joined = true;
            int checked_replies = 0;
            int checked_leaves = 0;
            {
                std::unique_lock lock (current ().mutex);
                completed = current ().cross_progress.wait_until (lock, registration_deadline, [&] {
                    return std::count (current ().notes.begin (), current ().notes.end (),
                                       "actorJoin:accepted")
                             >= 4
                           && std::count (current ().order.begin (), current ().order.end (),
                                          "joinReply:cross-a")
                                == 1
                           && std::count (current ().order.begin (), current ().order.end (),
                                          "joinReply:cross-b")
                                == 1
                           && std::count_if (current ().order.begin (), current ().order.end (),
                                             [] (const auto &event) {
                                                 return event.starts_with ("leaveCompleted:");
                                             })
                                == 2;
                });
                std::set<std::string> joined;
                for (const auto &event : current ().order) {
                    if (event.starts_with ("leaveCompleted:"))
                        ++checked_leaves;
                    if (event.starts_with ("joinedDone:")) {
                        joined.insert (event.substr (11));
                    } else if (event.starts_with ("joinReply:")) {
                        replies_after_joined &= joined.contains (event.substr (10));
                        ++checked_replies;
                    }
                }
            }
            actual["crossJoins"] =
              completed && replies_after_joined && checked_replies == 2 && checked_leaves == 2
                ? "completed"
                : "stalled";
            current ().cross_join_active.store (false);
            return;
        }
        if (act == "getOrCreateDuringClose") {
            auto &manager = services.get_required<zf::spot_manager_t> ();
            auto entered = current ().closing_entered.get_future ();
            auto closing =
              std::async (std::launch::async, [&] { return manager_close (services, *ref); });
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (entered.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("OnClosing did not start");
            const auto mismatch = manager.get_or_create (zf::spot_id_t (spot_id), "other-room")
                                    .timeout (3s)
                                    .async ()
                                    .result ();
            EXPECT_FALSE (mismatch);
            EXPECT_EQ (mismatch.error_kind (), zf::framework_error_kind_t::type_mismatch);
            auto pending = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                             .timeout (given.value ("deadlineExpires", false) ? 100ms : 3s)
                             .async ();
            if (given.value ("deadlineExpires", false)) {
                const auto result = pending.result ();
                actual["deadlineExceeded"] =
                  !result && result.error_kind () == zf::framework_error_kind_t::deadline_exceeded;
            } else {
                actual["completedBeforeRelease"] = pending.await_ready ();
                current ().closing_released.complete (zf::result_t<void>::success ());
                const auto closed = closing.get ();
                actual["closeSucceeded"] = closed && closed.value ();
                const auto result = pending.result ();
                actual["created"] =
                  result && result.value ().state == zf::spot_create_state_t::created;
                actual["newIncarnation"] =
                  result && result.value ().spot.object_generation () != ref->object_generation ();
                return;
            }
            current ().closing_released.complete (zf::result_t<void>::success ());
            const auto closed = closing.get ();
            actual["closeSucceeded"] = closed && closed.value ();
            return;
        }
        if (act == "sameRefCloseJoins") {
            auto &manager = services.get_required<zf::spot_manager_t> ();
            const auto existing = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                    .timeout (3s)
                                    .async ()
                                    .result ();
            if (!existing || existing.value ().state != zf::spot_create_state_t::existing
                || existing.value ().spot.object_generation () != ref->object_generation ())
                throw std::runtime_error ("Ready Existing incarnation was not observed");
            const auto existing_ref = existing.value ().spot;
            const auto state = current ().room_state;
            const auto owner = state->lane_owner.lock ();
            if (!owner)
                throw std::runtime_error ("Existing Spot owner is unavailable");
            const bool hold_delete = given.value ("holdDelete", false);
            if (hold_delete)
                store.hold_authority_release_once.store (true);
            auto held = hold_delete ? store.commit_held.get_future ()
                                    : current ().closing_entered.get_future ();
            auto first = manager.close (existing_ref);
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                current ().closing_released.complete (zf::result_t<void>::success ());
                store.release_held_commit (false);
            });
            if (held.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("Close did not reach its held stage");
            actual["authorityDuringClose"] = authority_state (services, spot_id);
            if (given.value ("otherGeneration", false)) {
                const auto generation = existing_ref.object_generation ();
                const auto other_ref = zf::spot_ref_t (
                  existing_ref.spot_id (), generation == 1 ? 2 : generation - 1,
                  std::string (existing_ref.mesh_name ()), existing_ref.node_rid ());
                auto other = manager.close (other_ref);
                (void) wait_until ([&] {
                    return other.await_ready ()
                           || owner->lane
                                .run ([&] { return !state->merged_close_results.empty (); })
                                .get ();
                });
                actual["otherCompletedBeforeRelease"] = other.await_ready ();
                current ().closing_released.complete (zf::result_t<void>::success ());
                store.release_held_commit (false);
                actual["firstResult"] = close_result (first.result ());
                actual["otherResult"] = close_result (other.result ());
                return;
            }
            auto second = manager.close (existing_ref);
            // Close는 공개 manager로 요청하며, owner 관찰은 합류 도착만 확인합니다.
            const auto merged = [&] {
                return owner->lane.run ([&] { return !state->merged_close_results.empty (); })
                  .get ();
            };
            (void) wait_until ([&] { return merged () || second.await_ready (); });
            actual["joinedBeforeRelease"] = merged ();
            actual["firstCompletedBeforeRelease"] = first.await_ready ();
            actual["secondCompletedBeforeRelease"] = second.await_ready ();
            current ().closing_released.complete (zf::result_t<void>::success ());
            store.release_held_commit (false);
            const auto first_result = first.result ();
            const auto second_result = second.result ();
            actual["firstResult"] = close_result (first_result);
            actual["secondResult"] = close_result (second_result);
            actual["sameResult"] =
              first_result && second_result && first_result.value () == second_result.value ();
            actual["thirdResult"] = close_result (manager.close (existing_ref).result ());
            return;
        }
        if (act == "joinQueuedBehindClose") {
            auto &actors = services.get_required<zf::actor_manager_t> ();
            const auto actor =
              actors.get_or_create (zf::actor_id_t ("queued-close-member"), "member")
                .timeout (3s)
                .async ()
                .result ();
            if (!actor)
                throw std::runtime_error ("queued Join Actor creation failed");
            const auto actor_ref = std::visit (
              [] (const auto &created) -> zf::actor_ref_t {
                  if constexpr (requires { created.actor; })
                      return created.actor;
                  else
                      throw std::runtime_error ("queued Join Actor creation was rejected");
              },
              actor.value ());
            auto &reader = services.get_required<zf::runtime::live_location_reader_t> ();
            const auto authority_read =
              reader.read_authority (zf::runtime::actor_authority_key ("queued-close-member"))
                .result ();
            const auto authority = std::get<zf::authority_snapshot_t> (authority_read.value ());
            const auto context = current ().room_state;
            const auto node = context->lane_owner.lock ();
            auto runtime = zf::detail::spot_node_runtime_t (node);
            std::promise<void> occupied, release;
            auto occupied_seen = occupied.get_future ();
            auto released = release.get_future ().share ();
            if (!node->worker_executor->try_submit_internal ([&] {
                    occupied.set_value ();
                    released.wait ();
                }))
                throw std::runtime_error ("worker hold was rejected");
            occupied_seen.get ();
            bool worker_released = false;
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                if (!worker_released)
                    release.set_value ();
            });
            auto close_done = std::make_shared<zf::task_completion_source_t<bool>> ();
            auto closing = close_done->task ();
            context->request_close ({}, [close_done] (zf::result_t<bool> result) {
                close_done->complete (std::move (result));
            });
            auto join_done = std::make_shared<zf::task_completion_source_t<void>> ();
            auto joining = join_done->task ();
            const bool remote = given.value ("remote", false);
            runtime.dispatch_wire_actor_join_admission (
              zf::spot_id_t (spot_id), {},
              [&, actor_ref] () -> zf::task_t<void> {
                  return join_queued_behind_close_task (&actual, node, spot_id, remote, runtime,
                                                        actor_ref, authority);
              },
              [join_done] (zf::result_t<void> result) { join_done->complete (std::move (result)); },
              [join_done] { join_done->complete (zf::result_t<void>::success ()); });
            actual["closePendingBeforeRelease"] = !closing.await_ready ();
            actual["joinPendingBeforeRelease"] = !joining.await_ready ();
            worker_released = true;
            release.set_value ();
            const auto closed = closing.result ();
            joining.result ().value ();
            actual["closeResult"] = close_result (closed);
            return;
        }
        if (act == "deferredJoinFailureContract") {
            const auto target = given.at ("target").get<std::string> ();
            const bool remove_after_resolution = given.value ("removeAfterResolution", false);
            const bool warm = given.at ("warmRoute").get<bool> ();
            store.capture_ready = true;
            if (authority_state (services, spot_id) != "Ready")
                throw std::runtime_error ("Ready authority was not observed");
            store.capture_ready = false;
            const auto join_id = target == "missing" ? "missing-" + spot_id : spot_id;
            if (warm) {
                const auto probe = direct_request (join_id, services, 1);
                if (target == "missing") {
                    if (probe || probe.error_kind () != zf::framework_error_kind_t::not_found)
                        throw std::runtime_error ("Missing warm-up must be NotFound");
                } else if (!probe)
                    throw std::runtime_error ("Ready warm-up failed");
            }
            auto &actors = services.get_required<zf::actor_manager_t> ();
            auto created =
              actors.get_or_create (zf::actor_id_t ("closing-admission-member"), "member")
                .timeout (3s)
                .async ()
                .result ();
            if (!created || !std::holds_alternative<zf::actor_create_created_t> (created.value ()))
                throw std::runtime_error ("Actor was not created");
            auto joined = current ().closing_join_completed.get_future ();
            auto entered = current ().closing_entered.get_future ();
            auto closing =
              std::async (std::launch::async, [&] { return manager_close (services, *ref); });
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                store.replay_ready = false;
                store.release_held_read ();
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (entered.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("OnClosing did not start");
            if (target != "closing") {
                current ().closing_released.complete (zf::result_t<void>::success ());
                const auto closed = closing.get ();
                if (!closed || !closed.value ())
                    throw std::runtime_error ("Close did not complete");
                store.replay_ready = target == "absent";
            } else {
                actual["authorityDuringClose"] = authority_state (services, spot_id);
            }
            const auto existing =
              actors.get_or_create (zf::actor_id_t ("closing-admission-member"), "member")
                .timeout (3s)
                .async ()
                .result ();
            if (!existing
                || !std::holds_alternative<zf::actor_create_existing_t> (existing.value ()))
                throw std::runtime_error ("Actor GetOrCreate did not return Existing");
            store.hold_resolved_read_once = remove_after_resolution;
            join_member (services, join_id, "closing-admission-member");
            if (remove_after_resolution) {
                auto held = store.read_held.get_future ();
                if (held.wait_for (3s) != std::future_status::ready)
                    throw std::runtime_error ("Closing authority read was not held");
                EXPECT_NE (joined.wait_for (0s), std::future_status::ready);
                current ().closing_released.complete (zf::result_t<void>::success ());
                const auto closed = closing.get ();
                if (!closed || !closed.value ())
                    throw std::runtime_error ("Close did not remove the activation");
                store.release_held_read ();
            }
            if (joined.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("Join completion did not arrive");
            const auto completion = joined.get ();
            const auto failed = std::get_if<zf::actor_join_failed_t> (&completion);
            actual["joinResult"] = failed ? error_name (failed->error_kind) : "notFailed";
            if (failed)
                EXPECT_TRUE (failed->operation_id_high != 0 || failed->operation_id_low != 0);
            store.replay_ready = false;
            if (given.value ("repeatAfterFailure", false)) {
                auto next = [&] {
                    std::lock_guard lock (current ().mutex);
                    current ().closing_join_completed =
                      std::promise<zf::actor_join_completion_t> ();
                    return current ().closing_join_completed.get_future ();
                }();
                join_member (services, join_id, "closing-admission-member");
                if (next.wait_for (3s) != std::future_status::ready)
                    throw std::runtime_error ("Second Join completion did not arrive");
                const auto next_completion = next.get ();
                const auto next_failed = std::get_if<zf::actor_join_failed_t> (&next_completion);
                actual["nextJoinResult"] =
                  next_failed ? error_name (next_failed->error_kind) : "notFailed";
            }
            if (target == "closing" && !remove_after_resolution) {
                current ().closing_released.complete (zf::result_t<void>::success ());
                const auto closed = closing.get ();
                if (!closed || !closed.value ())
                    throw std::runtime_error ("Close did not complete");
            }
            return;
        }
        if (act == "existingThenClosingAdmission") {
            auto &manager = services.get_required<zf::spot_manager_t> ();
            const auto existing = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                    .timeout (3s)
                                    .async ()
                                    .result ();
            if (!existing || existing.value ().state != zf::spot_create_state_t::existing
                || existing.value ().spot.object_generation () != ref->object_generation ())
                throw std::runtime_error ("Ready Existing incarnation was not observed");
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            auto entered = current ().closing_entered.get_future ();
            auto joined = current ().closing_join_completed.get_future ();
            auto closing =
              std::async (std::launch::async, [&] { return manager_close (services, *ref); });
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (entered.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("OnClosing did not start");
            actual["authorityDuringClose"] = authority_state (services, spot_id);
            const auto reply = direct_request (spot_id, services, 1);
            actual["requestResult"] = reply ? "handlerReply" : error_name (reply.error_kind ());
            join_member (services, spot_id, "closing-admission-member");
            if (joined.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("Join completion did not arrive during OnClosing");
            const auto completion = joined.get ();
            const auto failed = std::get_if<zf::actor_join_failed_t> (&completion);
            actual["joinResult"] = failed ? error_name (failed->error_kind) : "notFailed";
            current ().closing_released.complete (zf::result_t<void>::success ());
            const auto closed = closing.get ();
            actual["closeSucceeded"] = closed && closed.value ();
            return;
        }
        if (act == "directRequestDuringClose") {
            // The first request installs the Ready route in the source cache,
            // so the second one reaches the owner while its Close runs.
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
                current ().factory_calls = 0;
            }
            auto entered = current ().closing_entered.get_future ();
            std::thread closing ([&] { (void) manager_close (services, *ref); });
            if (entered.wait_for (3s) != std::future_status::ready) {
                current ().closing_released.complete (zf::result_t<void>::success ());
                closing.join ();
                throw std::runtime_error ("OnClosing did not start");
            }
            const auto authority_during = authority_state (services, spot_id);
            const auto reply = direct_request (spot_id, services, 1);
            current ().closing_released.complete (zf::result_t<void>::success ());
            closing.join ();
            actual["authorityDuringClose"] = authority_during;
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["authority"] = authority_state (services, spot_id);
            return;
        }
        if (act == "directRequestWithoutInstanceIntent") {
            if (given.at ("runtime") == "Draining") {
                stopping = true;
                (void) _app->shutdown (5s);
                (void) wait_until ([&] {
                    return _app->runtime_state () == zf::framework_runtime_state_t::draining;
                });
            }
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["creationIntent"] = false;
            return;
        }
        if (act == "directRequestDuringHeldClosingCommit") {
            // The request reaches the owner while the Closing commit is pending.
            // The lifecycle item holds application execution until the CAS result.
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            store.hold_closing_commit_once.store (true);
            auto held = store.commit_held.get_future ();
            std::optional<zf::result_t<bool>> closed;
            std::thread closing ([&] { closed.emplace (manager_close (services, *ref)); });
            if (held.wait_for (3s) != std::future_status::ready) {
                store.hold_closing_commit_once.store (false);
                closing.join ();
                throw std::runtime_error ("the Closing commit was not held");
            }
            std::future<void> admitted;
            {
                std::lock_guard lock (current ().mutex);
                current ().queued_request_admitted.emplace ();
                admitted = current ().queued_request_admitted->get_future ();
            }
            auto pending = std::async (std::launch::async, [&] {
                const auto flow = zf::runtime::flow_context_t::enter (
                  std::optional<std::string> (queued_request_flow_id),
                  zf::flow_origin_t::application, zf::message_flow_log_mode_t::detailed,
                  zf::flow_origin_t::application, std::nullopt);
                return direct_request (spot_id, services, 1);
            });
            if (admitted.wait_for (3s) != std::future_status::ready) {
                store.release_held_commit (true);
                closing.join ();
                (void) pending.get ();
                throw std::runtime_error ("queued request admission was not observed");
            }
            {
                std::lock_guard lock (current ().mutex);
                actual["handlerCallsBeforeCommit"] = current ().handler_calls;
            }
            store.release_held_commit (given.value ("closingCommit", "") == "heldThenConflict");
            const auto reply = pending.get ();
            closing.join ();
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["closeResult"] = close_result (*closed);
            actual["authority"] = authority_state (services, spot_id);
            if (authority_state (services, spot_id) == "Ready") {
                const auto admitted = direct_request (spot_id, services, 5);
                actual["admission"] = admitted ? "open" : "sealed";
            }
            return;
        }
        if (given.value ("closeGeneration", "") == "previous") {
            // A predecessor generation exists once the same Spot ID is closed
            // and created again; the setup Close is not part of the observation.
            if (manager_close (services, *ref).value () != true)
                throw std::runtime_error ("the setup Close did not close the first generation");
            ref = create_room (services, spot_id);
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        const auto close_ref = [&] {
            if (ref && given.value ("closeGeneration", "") == "previous") {
                if (ref->object_generation () <= 1)
                    throw std::runtime_error (
                      "the scenario needs a SpotRef whose generation has a predecessor");
                return zf::spot_ref_t (ref->spot_id (), ref->object_generation () - 1,
                                       std::string (ref->mesh_name ()), ref->node_rid ());
            }
            if (ref)
                return *ref;
            return zf::spot_ref_t (zf::spot_id_t (spot_id), 1, mesh_name,
                                   zf::node_rid_t::from_string (node_name));
        }();
        if (act == "managerClose") {
            {
                std::lock_guard lock (current ().mutex);
                current ().factory_calls = 0;
            }
            actual["result"] = close_result (manager_close (services, close_ref));
            if (given.value ("members", 0) > 0) {
                auto &manager = services.get_required<zf::spot_manager_t> ();
                const auto existing = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                        .timeout (3s)
                                        .async ()
                                        .result ();
                ASSERT_TRUE (existing);
                EXPECT_EQ (existing.value ().state, zf::spot_create_state_t::existing);
                EXPECT_EQ (existing.value ().spot.object_generation (),
                           close_ref.object_generation ());
            }
        } else if (act == "managerCloseThenManagerCloseAgain") {
            const auto first = manager_close (services, close_ref);
            actual["results"] = nlohmann::json::array (
              {first ? nlohmann::json (first.value ()) : nlohmann::json ("failure")});
            actual["authorityAfterFailure"] = authority_state (services, spot_id);
            actual["results"].push_back (close_result (manager_close (services, close_ref)));
        } else if (act == "joinAcceptedThenManagerClose") {
            const auto admitted_before = [&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted;
            }();
            std::thread join ([&] { join_member (services, spot_id, "member-" + name); });
            const bool admitted = wait_until ([&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted > admitted_before;
            });
            const auto closed = manager_close (services, close_ref);
            current ().record ("closeCompleted");
            join.join ();
            if (!admitted)
                throw std::runtime_error ("the Join was not accepted before Close");
            actual["result"] = close_result (closed);
        } else {
            throw std::runtime_error ("unsupported act: " + act);
        }
        actual["authority"] = authority_state (services, spot_id);
        if (ref && authority_state (services, spot_id) == "Ready") {
            const auto admitted = direct_request (spot_id, services, 5);
            actual["admission"] = admitted ? "open" : "sealed";
        }
    }

  public:
    std::shared_ptr<close_fault_store_t> _store;

  private:
    zf::app_t *_app;
    nlohmann::json _scenario;
};

const nlohmann::json &spot_close_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error ("Spot Close conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

const std::set<std::string> &known_scenarios ()
{
    static const std::set<std::string> names{
      "close-absent-incarnation-is-false",
      "close-other-generation-is-invalid-operation",
      "close-with-membership-is-false-and-keeps-authority",
      "close-after-accepted-join-observes-its-membership",
      "failure-before-closing-commit-keeps-authority",
      "on-closing-failure-is-diagnostic-and-cleanup-continues"};
    return names;
}

nlohmann::json run_scenario (const nlohmann::json &scenario, std::string &failure)
{
    // The observer is owned by the runtime and can fire after this scope ends.
    const auto observation_owner = std::make_shared<observation_t> ();
    observed = observation_owner.get ();

    const auto observe_diagnostics = [observation_owner] (const zf::message_flow_event_t &event) {
        auto &state = *observation_owner;
        if (event.outcome == zf::message_flow_outcome_t::admitted
            && event.packet_name == std::optional<std::string> (close_probe_request_t::packet_name)
            && event.detail_stage == std::optional<std::string> ("invoke_erased.post_serial")
            && event.flow_id == std::optional<std::string> (queued_request_flow_id)) {
            std::lock_guard lock (state.mutex);
            if (state.queued_request_admitted) {
                state.queued_request_admitted->set_value ();
                state.queued_request_admitted.reset ();
            }
        }
        if (event.result == zf::message_flow_result_t::failed) {
            std::string note =
              "flow:" + event.packet_name.value_or ("") + ":" + event.detail_result.value_or ("");
            if (event.exception) {
                try {
                    std::rethrow_exception (event.exception);
                }
                catch (const std::exception &error) {
                    note += std::string (":") + error.what ();
                }
                catch (...) {
                    note += ":non-standard-exception";
                }
            }
            std::lock_guard lock (state.mutex);
            state.notes.push_back (std::move (note));
        }
        if (event.packet_name == std::optional<std::string> ("spot_close")
            && event.detail_result == std::optional<std::string> ("on_closing_failed")) {
            std::lock_guard lock (state.mutex);
            state.diagnostics.push_back ("onClosingFailed");
        }
    };

    auto inner = std::make_shared<zf::runtime::in_memory_location_store_t> ();
    auto store = std::make_shared<close_fault_store_t> (inner);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    app.logging ().use_file ("spot-close.flow").set_min_level (zf::log_level_t::debug);
    app.add_zlink_framework ([&] (zf::zlink_framework_options_t &options) {
        if (scenario.at ("act") == "timerDuringPreparedAuthorityWrite"
            || scenario.at ("act") == "joinQueuedBehindClose")
            options.worker ().min_threads (1).max_threads (1);
        options.configure_dispatch ().message_flow (zf::message_flow_log_mode_t::detailed);
        zf::detail::dispatch_options_access_t::set_observer_for_tests (
          options.configure_dispatch (), observe_diagnostics);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        options.add_route_mesh (mesh_name)
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (std::string (node_name)))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<close_entry_spot_t> ([] (zf::entry_spot_context_t context) {
              return std::make_shared<close_entry_spot_t> (std::move (context));
          })
          .add_spot_factory<close_room_spot_t> (
            "room",
            [] (zf::spot_context_t context) {
                {
                    std::lock_guard lock (current ().mutex);
                    ++current ().factory_calls;
                }
                return std::make_shared<close_room_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_spot_factory<close_other_room_spot_t> (
            "other-room",
            [] (zf::spot_context_t context) {
                return std::make_shared<close_other_room_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_instance_spot_factory<close_session_spot_t> (
            "session",
            [] (zf::instance_spot_context_t context) {
                {
                    std::lock_guard lock (current ().mutex);
                    ++current ().factory_calls;
                }
                return std::make_shared<close_session_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<member_actor_t, member_actor_factory_t> (
            "member", std::make_shared<member_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto client = std::make_unique<scenario_client_t> (app, scenario);
    client->_store = store;
    auto *runner = client.get ();
    app.add_hosted_service (std::move (client));
    (void) app.run (0, nullptr);
    runner->join ();

    failure = runner->failure;
    auto actual = runner->actual;
    observed = nullptr;
    return actual;
}

void run_and_check (const nlohmann::json &scenario)
{
    const auto name = scenario.at ("name").get<std::string> ();
    SCOPED_TRACE (name);
    std::string failure;
    const auto actual = run_scenario (scenario, failure);
    std::cout << "spot-close scenario " << name << ": " << actual.dump () << std::endl;
    if (!failure.empty ()) {
        ADD_FAILURE () << failure << ": " << actual.dump ();
        return;
    }
    const auto &expect = scenario.at ("expect");
    for (const auto &[key, value] : expect.items ()) {
        if (key == "order") {
            // The expected events appear in this relative order.
            std::vector<std::string> seen;
            for (const auto &event : actual.at ("order"))
                if (std::find (value.begin (), value.end (), event) != value.end ())
                    seen.push_back (event.get<std::string> ());
            EXPECT_EQ (value.get<std::vector<std::string>> (), seen) << actual.dump ();
            continue;
        }
        if (!actual.contains (key)) {
            ADD_FAILURE () << key << " not observed: " << actual.dump ();
            continue;
        }
        EXPECT_EQ (value, actual.at (key)) << key << ": " << actual.dump ();
    }
}
} // namespace zlink::framework::test::spot_close
