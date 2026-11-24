/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/m6b_cases.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace spots = zlink::framework::detail;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
void verify_message_follow_invalidation_subscriptions_are_lifetime_safe ()
{
    using namespace zlink::framework;

    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("message-follow-subscription-mesh");
    auto runtime = std::make_shared<detail::mesh_node_runtime_t> (state);
    runtime->configure_actor_route_resolver (
      [] (actor_ref_t)
        -> zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>> {
          co_return std::nullopt;
      },
      [] (const auto &) { throw std::runtime_error ("expected invalidator failure"); });
    const auto route =
      protocol::actor_route_fence_t{"subscription-actor",
                                    1,
                                    zlink::routing_id_t::from ("subscription-source").to_bytes (),
                                    1,
                                    1,
                                    1};
    const auto notice = protocol::message_follow_notice_t{
      .source = route, .target = route, .original_operation = {1, 1}};

    std::atomic<std::size_t> duplicate_calls{0};
    const auto throwing = runtime->subscribe_message_follow_invalidation (
      [] (const auto &) { throw std::runtime_error ("expected subscriber failure"); });
    const auto duplicate_handler = [&] (const auto &) {
        duplicate_calls.fetch_add (1, std::memory_order_relaxed);
    };
    const auto first = runtime->subscribe_message_follow_invalidation (duplicate_handler);
    const auto second = runtime->subscribe_message_follow_invalidation (duplicate_handler);
    assert (first != 0 && second != 0 && first != second);
    runtime->dispatch_message_follow (notice);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 2);
    runtime->unsubscribe_message_follow_invalidation (throwing);

    runtime->unsubscribe_message_follow_invalidation (second);
    runtime->unsubscribe_message_follow_invalidation (second);
    runtime->dispatch_message_follow (notice);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 3);

    std::mutex callback_mutex;
    std::condition_variable callback_changed;
    bool callback_entered = false;
    bool callback_release = false;
    std::atomic<std::size_t> blocking_calls{0};
    const auto blocking = runtime->subscribe_message_follow_invalidation ([&] (const auto &) {
        blocking_calls.fetch_add (1, std::memory_order_relaxed);
        std::unique_lock lock (callback_mutex);
        callback_entered = true;
        callback_changed.notify_all ();
        callback_changed.wait (lock, [&] { return callback_release; });
    });
    auto dispatch =
      std::async (std::launch::async, [&] { runtime->dispatch_message_follow (notice); });
    {
        std::unique_lock lock (callback_mutex);
        assert (callback_changed.wait_for (lock, 2s, [&] { return callback_entered; }));
    }
    auto unsubscribe = std::async (
      std::launch::async, [&] { runtime->unsubscribe_message_follow_invalidation (blocking); });
    assert (unsubscribe.wait_for (20ms) == std::future_status::timeout);
    {
        std::lock_guard lock (callback_mutex);
        callback_release = true;
    }
    callback_changed.notify_all ();
    assert (dispatch.wait_for (2s) == std::future_status::ready);
    dispatch.get ();
    assert (unsubscribe.wait_for (2s) == std::future_status::ready);
    unsubscribe.get ();

    runtime->dispatch_message_follow (notice);
    assert (blocking_calls.load (std::memory_order_relaxed) == 1);
    assert (duplicate_calls.load (std::memory_order_relaxed) == 5);

    detail::mesh_node_runtime_t::message_follow_subscription_id_t self = 0;
    std::atomic<std::size_t> self_calls{0};
    self = runtime->subscribe_message_follow_invalidation ([&] (const auto &) {
        self_calls.fetch_add (1, std::memory_order_relaxed);
        runtime->unsubscribe_message_follow_invalidation (self);
    });
    runtime->dispatch_message_follow (notice);
    runtime->dispatch_message_follow (notice);
    assert (self_calls.load (std::memory_order_relaxed) == 1);

    auto second_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> (
      "message-follow-subscription-second-mesh");
    auto second_runtime = std::make_shared<detail::mesh_node_runtime_t> (second_state);
    std::atomic<std::size_t> multi_mesh_calls{0};
    const auto multi_first = runtime->subscribe_message_follow_invalidation (
      [&] (const auto &) { multi_mesh_calls.fetch_add (1, std::memory_order_relaxed); });
    const auto multi_second = second_runtime->subscribe_message_follow_invalidation (
      [&] (const auto &) { multi_mesh_calls.fetch_add (1, std::memory_order_relaxed); });
    runtime->dispatch_message_follow (notice);
    second_runtime->dispatch_message_follow (notice);
    assert (multi_mesh_calls.load (std::memory_order_relaxed) == 2);
    runtime->unsubscribe_message_follow_invalidation (multi_first);
    second_runtime->unsubscribe_message_follow_invalidation (multi_second);
    runtime->unsubscribe_message_follow_invalidation (first);
}

void verify_message_follow_unsubscribe_waits_for_capture_destruction ()
{
    using namespace zlink::framework;

    struct destruction_gate_t
    {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future ().share ();
    };
    struct captured_resource_t
    {
        std::shared_ptr<destruction_gate_t> gate;

        ~captured_resource_t ()
        {
            gate->entered.set_value ();
            gate->released.wait ();
        }
    };

    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("message-follow-capture-drain-mesh");
    auto runtime = std::make_shared<detail::mesh_node_runtime_t> (state);
    const auto route = protocol::actor_route_fence_t{
      "message-follow-capture-drain",
      1,
      zlink::routing_id_t::from ("message-follow-capture-source").to_bytes (),
      1,
      1,
      1};
    const auto notice = protocol::message_follow_notice_t{
      .source = route, .target = route, .original_operation = {1, 1}};

    auto gate = std::make_shared<destruction_gate_t> ();
    auto captured_resource = std::make_shared<captured_resource_t> ();
    captured_resource->gate = gate;
    std::future<void> capture_destruction_entered = gate->entered.get_future ();
    std::promise<void> callback_entered;
    std::promise<void> release_callback;
    const auto callback_released = release_callback.get_future ().share ();
    std::atomic<std::size_t> calls{0};
    const auto subscription = runtime->subscribe_message_follow_invalidation (
      [captured_resource = std::move (captured_resource), &callback_entered, callback_released,
       &calls] (const auto &) {
          if (calls.fetch_add (1, std::memory_order_relaxed) == 0) {
              callback_entered.set_value ();
              callback_released.wait ();
          }
      });

    auto dispatch =
      std::async (std::launch::async, [&] { runtime->dispatch_message_follow (notice); });
    const bool callback_started =
      callback_entered.get_future ().wait_for (2s) == std::future_status::ready;
    std::promise<void> unsubscribe_started;
    auto unsubscribe = std::async (std::launch::async, [&] {
        unsubscribe_started.set_value ();
        runtime->unsubscribe_message_follow_invalidation (subscription);
    });
    unsubscribe_started.get_future ().wait ();

    bool inactive_observed = false;
    const auto inactive_deadline = std::chrono::steady_clock::now () + 2s;
    while (callback_started && std::chrono::steady_clock::now () < inactive_deadline) {
        const auto before = calls.load (std::memory_order_relaxed);
        runtime->dispatch_message_follow (notice);
        if (calls.load (std::memory_order_relaxed) == before) {
            inactive_observed = true;
            break;
        }
    }

    release_callback.set_value ();
    const bool capture_destruction_started =
      capture_destruction_entered.wait_for (2s) == std::future_status::ready;
    const auto unsubscribe_status = unsubscribe.wait_for (50ms);
    gate->release.set_value ();
    const auto dispatch_status = dispatch.wait_for (2s);
    const auto final_unsubscribe_status = unsubscribe.wait_for (2s);
    if (dispatch_status == std::future_status::ready)
        dispatch.get ();
    if (final_unsubscribe_status == std::future_status::ready)
        unsubscribe.get ();

    assert (callback_started);
    assert (inactive_observed);
    assert (capture_destruction_started);
    assert (unsubscribe_status == std::future_status::timeout);
    assert (dispatch_status == std::future_status::ready);
    assert (final_unsubscribe_status == std::future_status::ready);
}
void verify_message_follow_route_admission_and_suppression ()
{
    using namespace zlink::framework;
    spots::actor_transfer_coordinator_t coordinator;
    const actor_ref_t target = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("node-b"), "player", "actor-message-follow", 7);
    const spot_route_t route{node_rid_t::from_string ("node-b"), "spot-b", "game"};
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "actor-message-follow", 7, zlink::routing_id_t::from ("node-a").to_bytes (), 11, 13, 17};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "actor-message-follow", 7, zlink::routing_id_t::from ("node-b").to_bytes (), 12, 16, 18};
    const auto expires = std::chrono::steady_clock::now () + 30s;
    coordinator.activate_message_follow ("player:actor-message-follow", source_fence, target, route,
                                         target_fence, expires, "relocation-1");
    assert (
      coordinator.matches_message_follow_source ("player:actor-message-follow", source_fence));
    auto wrong_source_fence = source_fence;
    ++wrong_source_fence.authority_owner_generation;
    assert (!coordinator.matches_message_follow_source ("player:actor-message-follow",
                                                        wrong_source_fence));
    const auto wrong_source = coordinator.try_acquire_message_follow ("player:actor-message-follow",
                                                                      7, 1, 0, wrong_source_fence);
    assert (!wrong_source && wrong_source.error_kind () == framework_error_kind_t::unavailable);
    const auto exact_source =
      coordinator.try_acquire_message_follow ("player:actor-message-follow", 7, 1, 0, source_fence);
    assert (exact_source && exact_source.value ());
    assert (exact_source.value ()->source_fence == source_fence);
    assert (exact_source.value ()->target_fence == target_fence);
    coordinator.release_message_follow ("player:actor-message-follow", source_fence, 1);

    const auto missing =
      coordinator.try_acquire_message_follow ("player:missing", 7, 1, 0, source_fence);
    assert (missing && !missing.value ());
    auto newer_generation_source = source_fence;
    newer_generation_source.object_generation = 8;
    const auto stale_generation = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, newer_generation_source);
    assert (!stale_generation
            && stale_generation.error_kind () == framework_error_kind_t::invalid_operation);
    const auto hop_bound =
      coordinator.try_acquire_message_follow ("player:actor-message-follow", 7, 1, 8, source_fence);
    assert (!hop_bound && hop_bound.error_kind () == framework_error_kind_t::unavailable);
    // Message Follow admission has no relocation-specific message-count or
    // stored-size bound. Keep the acquisitions concurrent so this checks the
    // former count boundary rather than repeatedly reusing one slot.
    const auto oversized = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 16u * 1024u * 1024u + 1u, 0, source_fence);
    assert (oversized && oversized.value ());
    coordinator.release_message_follow ("player:actor-message-follow", source_fence,
                                        16u * 1024u * 1024u + 1u);
    const auto maximum_sized = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, std::numeric_limits<std::size_t>::max (), 0, source_fence);
    assert (maximum_sized && maximum_sized.value ());
    const auto accounting_overflow =
      coordinator.try_acquire_message_follow ("player:actor-message-follow", 7, 1, 0, source_fence);
    assert (!accounting_overflow
            && accounting_overflow.error_kind () == framework_error_kind_t::internal_failure);
    coordinator.release_message_follow ("player:actor-message-follow", source_fence,
                                        std::numeric_limits<std::size_t>::max ());
    for (std::size_t index = 0; index != 2048; ++index) {
        const auto acquired = coordinator.try_acquire_message_follow ("player:actor-message-follow",
                                                                      7, 1, 0, source_fence);
        assert (acquired && acquired.value ());
    }
    for (std::size_t index = 0; index != 2048; ++index)
        coordinator.release_message_follow ("player:actor-message-follow", source_fence, 1);

    auto repeated_source_fence = source_fence;
    repeated_source_fence.authority_owner_generation = 19;
    repeated_source_fence.owner_lease_generation = 23;
    auto repeated_target_fence = target_fence;
    repeated_target_fence.authority_owner_generation = 24;
    repeated_target_fence.owner_lease_generation = 29;
    coordinator.activate_message_follow ("player:actor-message-follow", repeated_source_fence,
                                         target, route, repeated_target_fence, expires,
                                         "relocation-2");
    const auto first_retained_source =
      coordinator.try_acquire_message_follow ("player:actor-message-follow", 7, 1, 0, source_fence);
    const auto second_retained_source = coordinator.try_acquire_message_follow (
      "player:actor-message-follow", 7, 1, 0, repeated_source_fence);
    assert (first_retained_source && first_retained_source.value ());
    assert (second_retained_source && second_retained_source.value ());
    assert (first_retained_source.value ()->target_fence == target_fence);
    assert (second_retained_source.value ()->target_fence == repeated_target_fence);
    coordinator.release_message_follow ("player:actor-message-follow", source_fence, 1);
    coordinator.release_message_follow ("player:actor-message-follow", repeated_source_fence, 1);

    std::atomic_int notification_winners{0};
    std::vector<std::thread> notification_attempts;
    for (int attempt = 0; attempt != 8; ++attempt) {
        notification_attempts.emplace_back ([&] {
            if (coordinator.try_begin_message_follow_notification ("player:actor-message-follow",
                                                                   source_fence, target_fence)) {
                notification_winners.fetch_add (1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &attempt : notification_attempts)
        attempt.join ();
    assert (notification_winners.load (std::memory_order_relaxed) == 1);
    assert (!coordinator.try_begin_message_follow_notification ("player:actor-message-follow",
                                                                source_fence, target_fence));
    assert (coordinator.complete_message_follow_notification ("player:actor-message-follow",
                                                              source_fence, target_fence, false));
    assert (coordinator.try_begin_message_follow_notification ("player:actor-message-follow",
                                                               source_fence, target_fence));
    assert (coordinator.complete_message_follow_notification ("player:actor-message-follow",
                                                              source_fence, target_fence, true));
    assert (!coordinator.try_begin_message_follow_notification ("player:actor-message-follow",
                                                                source_fence, target_fence));
    assert (!coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, repeated_target_fence));

    auto replacement_target_fence = target_fence;
    ++replacement_target_fence.authority_owner_generation;
    ++replacement_target_fence.owner_lease_generation;
    coordinator.activate_message_follow ("player:actor-message-follow", source_fence, target, route,
                                         replacement_target_fence, expires,
                                         "relocation-1-replaced");
    assert (coordinator.try_begin_message_follow_notification (
      "player:actor-message-follow", source_fence, replacement_target_fence));
    assert (coordinator.complete_message_follow_notification (
      "player:actor-message-follow", source_fence, replacement_target_fence, true));
    assert (!coordinator.remove_message_follow ("player:actor-message-follow", source_fence,
                                                target_fence));
    assert (
      coordinator.matches_message_follow_source ("player:actor-message-follow", source_fence));
    const auto removed = coordinator.remove_message_follow ("player:actor-message-follow",
                                                            source_fence, replacement_target_fence);
    assert (removed && removed->transfer_id == "relocation-1-replaced");
    assert (
      !coordinator.matches_message_follow_source ("player:actor-message-follow", source_fence));
    assert (coordinator.matches_message_follow_source ("player:actor-message-follow",
                                                       repeated_source_fence));
    assert (!coordinator.remove_message_follow ("player:actor-message-follow", source_fence,
                                                replacement_target_fence));

    coordinator.activate_message_follow (
      "player:expired-message-follow", source_fence, target, route, target_fence,
      std::chrono::steady_clock::now () - 1ms, "relocation-expired");
    const auto expired = coordinator.try_acquire_message_follow ("player:expired-message-follow", 7,
                                                                 1, 0, source_fence);
    assert (!expired && expired.error_kind () == framework_error_kind_t::unavailable);
}
}
