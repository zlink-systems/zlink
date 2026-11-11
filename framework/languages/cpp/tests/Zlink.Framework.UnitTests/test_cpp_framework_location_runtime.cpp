/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace zlink::framework::detail
{
struct spot_node_runtime_test_access_t
{
    static task_t<void> commit_membership (spot_node_runtime_t &owner,
                                           spot_context_t &context,
                                           const actor_ref_t &actor)
    {
        bool committed = false;
        spot_actor_admission_callbacks_t callbacks;
        co_await owner.commit_accepted_actor_join ("player:early", context, actor, typeid (int),
                                                   nullptr, callbacks, false, {}, {}, committed,
                                                   {});
        EXPECT_TRUE (committed);
    }
};
}

namespace zlink::framework::runtime
{
constexpr auto auto_connect_wait = std::chrono::seconds (5);

namespace client_server
{
struct client_server_location_runtime_test_access_t
{
    static bool stop_requested (const client_server_location_runtime_t &runtime)
    {
        return runtime._stop->stop_requested ();
    }
};
} // namespace client_server

namespace fanout
{
struct fanout_location_runtime_test_access_t
{
    static bool stop_requested (const fanout_location_runtime_t &runtime)
    {
        return runtime._stop.stop_requested ();
    }
};
} // namespace fanout

namespace host
{
struct public_host_runtime_test_access_t
{
    template <class Host, class Publication>
    static task_t<stateful::stateful_error_t> adopt (Host &host, const Publication &published)
    {
        if constexpr (requires {
                          host.advance_local_actor_authority (published.current->target,
                                                              published.current->ready);
                      })
            return host.advance_local_actor_authority (published.current->target,
                                                       published.current->ready);
        else
            return host.advance_local_actor_authority (published.current->target);
    }
};
}

struct serial_execution_queue_test_access_t
{
    static void select_then (serial_execution_queue_t &queue, std::function<void ()> close)
    {
        auto selected = [&queue, close = std::move (close)] {
            serial_execution_queue_t::work_item_t item;
            {
                std::lock_guard lock (queue._mutex);
                item = queue.take_next_locked ();
                ++queue._active;
                queue._active_lane = item.lane;
                queue._active_bytes = item.byte_cost;
            }
            close ();
            if (item.actor_mailbox)
                queue.execute_actor_head (std::move (item));
            else
                queue.execute_item (std::move (item));
        };
        if (queue.owns_shared_gate ())
            queue.notify_control ("selected-before-pause", std::move (selected));
        else
            selected ();
    }
};

struct state_lane_test_access_t
{
    static std::size_t pending_work (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return lane._mailbox.size ();
    }
    static bool pending_synchronous_turn (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return !lane._mailbox.empty () && !lane._mailbox.front ().work;
    }
    static bool owns_turn (state_lane_t &lane)
    {
        return lane._scheduled.load (std::memory_order_acquire);
    }
};

struct auto_connect_cache_test_access_t
{
    static state_lane_t &lane (store_location_resolvers_t &cache) { return cache._lane; }

    static void seed (store_location_resolvers_t &cache)
    {
        cache._lane.run_sync ([&] {
            cache._spot_routes.try_emplace ("spot");
            cache._actor_routes.try_emplace ("actor");
        });
    }
    static void expect_recovered (store_location_resolvers_t &cache)
    {
        const auto snapshot = cache._lane.run_sync ([&] {
            return std::tuple (cache._spot_routes.size (), cache._actor_routes.size (),
                               cache._store_recovery_generation);
        });
        EXPECT_EQ (std::make_tuple (std::size_t{0}, std::size_t{0}, std::uint64_t{1}), snapshot);
    }
};

// Forward all Store operations to the real provider; inject one scan failure only.
class auto_connect_probe_store_t final : public location_store_t
{
  public:
    in_memory_location_store_t provider;
    bool fail_next_scan = false;
    unsigned reads = 0, writes = 0, scans = 0, failures = 0;

    task_t<store_read_result_t> read (store_key_t key) override
    {
        ++reads;
        return provider.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        ++writes;
        return provider.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        ++scans;
        if (std::exchange (fail_next_scan, false)) {
            ++failures;
            return task_t<store_scan_result_t> (result_t<store_scan_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected one-worker Store scan failure"));
        }
        return provider.scan (std::move (request));
    }
};

struct location_auto_connect_test_access_t
{
    struct provider_probe_t
    {
        auto_connect_probe_store_t &provider;
        provider_location_repository_t &store;
        location_runtime_t &runtime;
        detail::route_channel_runtime_t &route;
        mesh_node_descriptor_t peer;
        unsigned connects = 0, disconnects = 0, ticks = 0;
        bool later_wait_ready = false, saw_failure = false;
    };

    static task_t<void>
    provider_probe_update (location_auto_connect_host_service_t *service,
                           provider_probe_t *probe,
                           location_auto_connect_host_service_t::target_t target,
                           bool remove)
    {
        co_await service->update_route_target (service, &probe->route, {}, target, remove);
        if (remove) {
            ++probe->disconnects;
            EXPECT_EQ (location_write_status_t::stored,
                       (co_await probe->store.update_mesh_node (probe->peer,
                                                                location_write_intent_t::new_claim))
                         .status);
            probe->provider.fail_next_scan = true;
        } else if (++probe->connects == 1) {
            EXPECT_EQ (location_write_status_t::stored,
                       co_await probe->store.remove_mesh_node (
                         {probe->peer.mesh_name, probe->peer.rid},
                         {probe->peer.owner_id, probe->peer.lease_generation}));
        }
        // Request reconciliation before run_loop reaches its wait. This exercises
        // the ready lease-change path without a periodic timer or a sleep.
        probe->runtime.peer_reconcile_requester () ({});
        if (probe->ticks == 1) {
            probe->later_wait_ready =
              probe->runtime.wait_for_owner_lease_change (probe->runtime.owner_lease_observation ())
                .await_ready ();
            probe->runtime.peer_reconcile_requester () ({});
        }
    }

    static void configure_provider_probe (location_auto_connect_host_service_t &service,
                                          provider_probe_t &probe,
                                          live_location_reader_t &reader,
                                          store_location_resolvers_t &cache)
    {
        service._runtime = &probe.runtime;
        service._store = &probe.store;
        service._live_store = &reader;
        service._route_cache = &cache;
        service._republish_after_store_recovery = [&service, &probe] (auto, auto, auto) {
            ++probe.ticks;
            if (probe.ticks == 5)
                service._stop_requested.request_stop ();
            if (probe.ticks == 3) {
                probe.saw_failure = has_tick_failure (service);
                probe.runtime.peer_reconcile_requester () ({});
            }
            return task_t<bool> (result_t<bool>::success (true));
        };
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &probe] (auto target) {
              return provider_probe_update (&service, &probe, std::move (target), false);
          },
          [&service, &probe] (auto target) {
              return provider_probe_update (&service, &probe, std::move (target), true);
          },
          [&service, &probe] (const auto &target) {
              service.disconnect_route_target (&service, probe.route, {}, target);
          });
    }

    static location_auto_connect_host_service_t::target_t peer_target ()
    {
        location_auto_connect_host_service_t::target_t target;
        target.node_rid = zlink::routing_id_t::from ("remote");
        target.endpoint = "inproc://l4-peer";
        target.lifecycle_generation = 1;
        target.security_identity = "default";
        return target;
    }

    static task_t<void> route_target (location_auto_connect_host_service_t &service,
                                      detail::route_channel_runtime_t &route,
                                      bool remove)
    {
        return service.update_route_target (&service, &route, {}, peer_target (), remove);
    }

    static task_t<void> mesh_target (std::shared_ptr<detail::mesh_node_runtime_t> node, bool remove)
    {
        return location_auto_connect_host_service_t::update_mesh_target (std::move (node),
                                                                         peer_target (), remove);
    }

    static void register_stop_probe (location_auto_connect_host_service_t &service,
                                     detail::route_channel_runtime_t &route,
                                     std::shared_ptr<detail::mesh_node_runtime_t> node,
                                     bool mesh)
    {
        if (mesh)
            service.add_mesh_loop (node);
        else
            service.add_route_loop (route, std::nullopt, std::nullopt, false, {});
        service._loops.front ().active.emplace ("remote", peer_target ());
    }

    static task_t<void> finish_probe (location_auto_connect_host_service_t *service,
                                      detail::route_channel_runtime_t *route,
                                      location_auto_connect_host_service_t::target_t target)
    {
        co_await service->update_route_target (service, route, {}, std::move (target), false);
        service->_stop_requested.request_stop ();
    }

    static void check_stop_between_targets (location_auto_connect_host_service_t &service,
                                            int operation)
    {
        auto completion = std::make_shared<task_completion_source_t<void>> ();
        unsigned calls = 0;
        std::map<std::string, unsigned> disconnects;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&] (auto) {
              ++calls;
              return completion->task ();
          },
          [&] (auto target) {
              ++calls;
              ++disconnects[target.endpoint];
              return completion->task ();
          },
          [&] (const auto &target) { ++disconnects[target.endpoint]; });
        auto &loop = service._loops.front ();
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        for (auto name : {"remote", "remote-two"}) {
            auto target = peer_target ();
            target.key = name;
            target.node_rid = zlink::routing_id_t::from (name);
            auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
            descriptor->mesh_name = "l4-mesh";
            descriptor->rid = target.node_rid;
            descriptor->endpoint = target.endpoint + name;
            descriptor->owner_id = "remote-owner";
            descriptor->lease_generation = 1;
            descriptor->lifecycle_generation = 1;
            descriptor->state = framework_runtime_state_t::serving;
            target.original = descriptor;
            target.endpoint = descriptor->endpoint;
            sources->push_back (
              {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});
        }
        if (operation != 0)
            loop.active = service.compute_desired (loop, sources);
        if (operation == 1)
            sources->clear ();
        if (operation == 2)
            for (auto &source : *sources)
                source.descriptor.endpoint += "-replacement";
        auto pending = service.finish_loop (service.reconcile (&loop, sources, true));
        ASSERT_FALSE (pending.await_ready ());
        ASSERT_EQ (1u, calls);
        service._stop_requested.request_stop ();
        // Stop waits for the loop before cleaning active targets. Finish the
        // owner operation first, then model that cleanup after its effect is recorded.
        completion->complete (result_t<void>::success ());
        const auto result = pending.result_for (std::chrono::seconds (5));
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
        EXPECT_EQ (1u, calls);
        EXPECT_TRUE (loop.active.empty ());
        EXPECT_EQ (operation == 0 ? 1u : 2u, disconnects.size ());
        for (const auto &[endpoint, count] : disconnects) {
            SCOPED_TRACE (endpoint);
            EXPECT_EQ (1u, count);
        }
    }

    static void check_stop_after_pending_connect (location_auto_connect_host_service_t &service,
                                                  location_runtime_t &runtime,
                                                  detail::route_channel_runtime_t &route)
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        unsigned disconnects = 0;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &route] (auto target) {
              return service.update_route_target (&service, &route, {}, std::move (target), false);
          },
          [&service, &route] (auto target) {
              return service.update_route_target (&service, &route, {}, std::move (target), true);
          },
          [&service, &route, &disconnects] (const auto &target) {
              ++disconnects;
              service.disconnect_route_target (&service, route, {}, target);
          });

        auto &loop = service._loops.front ();
        auto target = peer_target ();
        auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
        descriptor->mesh_name = "l4-mesh";
        descriptor->rid = target.node_rid;
        descriptor->endpoint = target.endpoint;
        descriptor->owner_id = "remote-owner";
        descriptor->lease_generation = 1;
        descriptor->lifecycle_generation = 1;
        descriptor->state = framework_runtime_state_t::serving;
        target.key = descriptor->rid.to_string ();
        target.original = descriptor;
        loop.last_desired.emplace (target.key, target);
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        sources->push_back (
          {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});

        std::promise<void> owner_entered, release_owner;
        auto released = release_owner.get_future ().share ();
        auto &owner = auto_connect_owner_lane (route);
        auto held_owner = std::async (std::launch::async, [&] {
            owner.run_sync ([&] {
                owner_entered.set_value ();
                released.wait ();
            });
        });
        owner_entered.get_future ().wait ();

        auto pending = service.reconcile (&loop, sources, true);
        const auto owner_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (state_lane_test_access_t::pending_work (owner) == 0
               && std::chrono::steady_clock::now () < owner_deadline)
            std::this_thread::yield ();
        EXPECT_GT (state_lane_test_access_t::pending_work (owner), 0u);
        EXPECT_FALSE (pending.await_ready ());

        auto finished = service._loop_finished;
        auto loop_runner =
          std::async (std::launch::async, [pending = std::move (pending), finished] () mutable {
              const auto result = pending.result_for (auto_connect_wait);
              if (result)
                  finished->complete (result_t<void>::success ());
              else
                  finished->complete (result_t<void>::failure (
                    framework_error_kind_t::deadline_exceeded, "test reconcile did not finish"));
          });
        auto stopping = std::async (std::launch::async, [&] { service.stop (); });
        const auto stop_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (!service._stop_requested.stop_requested ()
               && std::chrono::steady_clock::now () < stop_deadline)
            std::this_thread::yield ();
        EXPECT_TRUE (service._stop_requested.stop_requested ());

        release_owner.set_value ();
        held_owner.get ();
        EXPECT_EQ (std::future_status::ready, loop_runner.wait_for (auto_connect_wait));
        loop_runner.get ();
        EXPECT_EQ (std::future_status::ready, stopping.wait_for (auto_connect_wait));
        stopping.get ();
        EXPECT_EQ (1u, disconnects);
        EXPECT_TRUE (route.list_connections ().empty ());
    }

    static task_t<void>
    pending_target (detail::route_channel_runtime_t *route,
                    location_auto_connect_host_service_t::target_t target,
                    std::shared_ptr<task_completion_source_t<void>> release,
                    bool remove,
                    std::shared_ptr<std::map<std::string, unsigned>> disconnects)
    {
        co_await release->task ();
        if (remove) {
            if (disconnects)
                ++(*disconnects)[target.endpoint];
            (void) route->disconnect (target.node_rid, target.endpoint);
        } else
            (void) route->connect (target.node_rid, target.endpoint);
    }

    static auto
    start_timeout_probe (location_auto_connect_host_service_t &service,
                         location_runtime_t &runtime,
                         detail::route_channel_runtime_t &route,
                         std::shared_ptr<task_completion_source_t<void>> release,
                         int operation,
                         std::shared_ptr<std::map<std::string, unsigned>> disconnects = {})
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&route, release, disconnects] (auto target) {
              return pending_target (&route, target, release, false, disconnects);
          },
          [&route, release, disconnects] (auto target) {
              return pending_target (&route, target, release, true, disconnects);
          },
          [&service, &route, disconnects] (const auto &target) {
              if (disconnects)
                  ++(*disconnects)[target.endpoint];
              service.disconnect_route_target (&service, route, {}, target);
          });
        auto &loop = service._loops.front ();
        auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
        descriptor->mesh_name = "l4-mesh";
        descriptor->rid = zlink::routing_id_t::from ("remote");
        descriptor->endpoint = "inproc://l4-timeout";
        descriptor->owner_id = "remote-owner";
        descriptor->lease_generation = 1;
        descriptor->lifecycle_generation = 1;
        descriptor->state = framework_runtime_state_t::serving;
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        sources->push_back (
          {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});
        if (operation != 0) {
            loop.active = service.compute_desired (loop, sources);
            (void) route.connect (descriptor->rid, descriptor->endpoint);
        }
        if (operation == 1)
            sources->clear ();
        if (operation == 2)
            sources->front ().descriptor.endpoint += "-replacement";
        auto pending = service.finish_loop (service.reconcile (&loop, sources, true));
        EXPECT_FALSE (pending.await_ready ());
        auto finished = service._loop_finished;
        detail::observe_task_terminal (
          pending, [finished] (const result_t<void> &result) { finished->complete (result); });
        return finished->task ();
    }

    static std::size_t loop_count (const location_auto_connect_host_service_t &service)
    {
        return service._loops.size ();
    }

    static void configure_probe (location_auto_connect_host_service_t &service,
                                 location_runtime_t &runtime,
                                 location_repository_t &store,
                                 live_location_reader_t &reader,
                                 detail::route_channel_runtime_t &route,
                                 unsigned &calls,
                                 bool later,
                                 store_location_resolvers_t *cache)
    {
        service._runtime = &runtime;
        service._store = &store;
        service._live_store = &reader;
        service._route_cache = cache;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &route, &calls, &runtime, later, cache] (const auto &target) {
              ++calls;
              if (cache) {
                  service._stop_requested.request_stop ();
                  return task_t<void> (result_t<void>::success ());
              }
              if (later && calls == 1) {
                  runtime.peer_reconcile_requester () ({});
                  return task_t<void> (result_t<void>::success ());
              }
              return finish_probe (&service, &route, target);
          },
          [] (const auto &) { return task_t<void> (result_t<void>::success ()); },
          [] (const auto &) {});
        service._loops.front ().recovering_from_store_failure = cache != nullptr;
    }

    static task_t<bool> publish_without_start (location_auto_connect_host_service_t &service,
                                               location_runtime_t &runtime)
    {
        service._runtime = &runtime;
        return service.republish_after_store_recovery (location_write_intent_t::renew);
    }

    static task_t<void> run_probe (location_auto_connect_host_service_t &service)
    {
        return service.run_loop (service._runtime->owner_lease_observation ());
    }

    static bool has_tick_failure (const location_auto_connect_host_service_t &service)
    {
        return service._loops.front ().failure_started_at.has_value ();
    }

    static auto hold_loop_completion (location_auto_connect_host_service_t &service,
                                      location_runtime_t &runtime)
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        return service._loop_finished;
    }

    static void add_test_loop (location_auto_connect_host_service_t &service,
                               std::function<void (std::string)> connected,
                               std::function<void ()> disconnected,
                               bool refresh_capacity = false)
    {
        service.add_loop (
          "ordered-mesh",
          refresh_capacity ? std::optional{zlink::routing_id_t::from ("local")} : std::nullopt, {},
          std::nullopt, false,
          [connected] (const auto &target) {
              connected (target.node_rid.to_string ());
              return task_t<void> (result_t<void>::success ());
          },
          [disconnected] (const auto &) {
              disconnected ();
              return task_t<void> (result_t<void>::success ());
          },
          [disconnected] (const auto &) { disconnected (); });
    }
};

struct mesh_node_host_service_test_access_t
{
    static void configure (mesh_node_host_service_t &service,
                           std::shared_ptr<location_repository_t> store,
                           location_runtime_t &runtime,
                           bool published = false)
    {
        service._location_store = std::move (store);
        service._location_runtime = &runtime;
        service._location_store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "publication-mesh";
        descriptor.rid = zlink::routing_id_t::from ("local");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "inproc://publication-mesh";
        descriptor.security_identity = "default";
        descriptor.activation_concurrency.limit = 1;
        if (published) {
            const auto owner = runtime.current_owner_token ().value ();
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            EXPECT_EQ (location_write_status_t::stored,
                       service._location_store
                         ->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            service._published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
        }
        service._published_mesh_descriptors.push_back (std::move (descriptor));
    }
    static void recovery_descriptors (mesh_node_host_service_t &service,
                                      location_repository_t &store,
                                      location_runtime_t &runtime,
                                      const mesh_node_descriptor_t &descriptor)
    {
        service._location_store = std::shared_ptr<location_repository_t> (&store, [] (auto *) {});
        service._location_runtime = &runtime;
        service._published_mesh_descriptors.push_back (descriptor);
        service._published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
        auto other = descriptor;
        other.rid = zlink::routing_id_t::from ("second-descriptor");
        EXPECT_EQ (location_write_status_t::stored,
                   store.update_mesh_node (other, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        service._published_mesh_descriptors.push_back (other);
        service._published_mesh_nodes.push_back ({other.mesh_name, other.rid});
    }
    static bool publication_mutex_available (mesh_node_host_service_t &service)
    {
        if (!service._descriptor_publish_mutex.try_lock ())
            return false;
        service._descriptor_publish_mutex.unlock ();
        return true;
    }
    // This adapter also compiles against the original synchronous implementation
    // for the RED run. Only its return shape is normalized, never its scheduling.
    template <typename Service>
    static task_t<bool> publish (Service &service,
                                 std::stop_token cancellation,
                                 std::chrono::steady_clock::time_point deadline)
    {
        if constexpr (requires {
                          service.republish_after_store_recovery (
                            location_write_intent_t::new_claim, cancellation, deadline);
                      })
            return service.republish_after_store_recovery (location_write_intent_t::new_claim,
                                                           cancellation, deadline);
        else
            return task_t<bool> (result_t<bool>::success (
              service.republish_after_store_recovery (location_write_intent_t::new_claim)));
    }
};

const auto requested_peer_rid = zlink::routing_id_t::from ("requested-peer");
} // namespace zlink::framework::runtime

namespace
{

using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

constexpr auto heartbeat_observation_timeout = std::chrono::seconds (5);
constexpr auto admission_observation_timeout = std::chrono::milliseconds (100);
const location_options_t asynchronous_heartbeat_options{
  .owner_lease_renew_interval = std::chrono::milliseconds (5),
  .owner_lease_ttl = std::chrono::seconds (15)};

class requested_peer_store_t final : public in_memory_location_repository_t
{
  public:
    std::atomic<unsigned> reads = 0;
    std::atomic<bool> visible = false;
    std::promise<void> first_read;
    std::latch release_first_read{1};
    zlink::framework::location_owner_token_t owner;

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh, zlink::framework::location_page_request_t = {}) override
    {
        const bool include_peer = visible.load ();
        if (++reads == 1) {
            first_read.set_value ();
            release_first_read.wait ();
        }
        zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t> page;
        if (include_peer) {
            zlink::framework::mesh_node_descriptor_t descriptor;
            descriptor.mesh_name = std::move (mesh);
            descriptor.rid = zlink::framework::runtime::requested_peer_rid;
            descriptor.endpoint = "inproc://requested-peer";
            descriptor.lifecycle_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            page.items.push_back (std::move (descriptor));
        }
        return zlink::framework::task_t<decltype (page)> (
          zlink::framework::result_t<decltype (page)>::success (std::move (page)));
    }
};

class held_descriptor_store_t final : public in_memory_location_repository_t
{
  public:
    zlink::framework::task_completion_source_t<zlink::framework::location_write_result_t> write;
    std::promise<void> entered;
    std::atomic<unsigned> publications = 0;

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t,
                      zlink::framework::location_write_intent_t) override
    {
        if (++publications == 1)
            entered.set_value ();
        return write.task ();
    }

    void release ()
    {
        write.complete (
          zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
            {zlink::framework::location_write_status_t::stored, 1, {}}));
    }
};

TEST (ZLinkFrameworkLocationRuntime, MeshPublicationSuspendsWithoutHoldingDescriptorMutex)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_descriptor_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::seconds (2);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    location_runtime_t runtime (*store, options);
    runtime.start (zlink::routing_id_t::from ("mesh-publication"));
    serializer_registry_t serializers;
    mesh_node_host_service_t service (std::make_shared<runtime_failure_collector_t> (), {},
                                      serializers);
    mesh_node_host_service_test_access_t::configure (service, store, runtime);
    std::stop_source stop;
    auto started = store->entered.get_future ();
    auto invocation = std::async (std::launch::async, [&] {
        return mesh_node_host_service_test_access_t::publish (
          service, stop.get_token (),
          std::chrono::steady_clock::now () + options.owner_lease_renew_timeout);
    });
    const auto started_status = started.wait_for (std::chrono::seconds (5));
    const auto returned = invocation.wait_for (std::chrono::milliseconds (100));
    const auto mutex_available =
      mesh_node_host_service_test_access_t::publication_mutex_available (service);
    stop.request_stop ();
    // The old method must also be released after RED is recorded.
    if (returned != std::future_status::ready)
        store->release ();
    auto publication = invocation.get ();
    const auto canceled = publication.result_for (options.owner_lease_renew_timeout);
    store->release ();
    EXPECT_EQ (std::future_status::ready, started_status);
    EXPECT_EQ (std::future_status::ready, returned);
    EXPECT_TRUE (mutex_available);
    ASSERT_TRUE (canceled.has_value ());
    ASSERT_TRUE (*canceled);
    EXPECT_FALSE (canceled->value ());
    EXPECT_EQ (1u, store->publications.load ());
    service.stop ();
    runtime.stop ();
}

// The provider controls completion; the timeout observes stop, never releases I/O.
TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingPublication)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("coordinator"));
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    task_completion_source_t<bool> write;
    std::promise<void> entered;
    auto started = entered.get_future ();
    std::atomic<unsigned> publications = 0;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (), {}, handlers,
      serializers, [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token cancellation,
           std::chrono::steady_clock::time_point deadline) -> task_t<bool> {
          ++publications;
          entered.set_value ();
          const auto result = co_await observe_until (write.task (), deadline, cancellation);
          co_return result &&result.value ();
      });
    service.start (provider).result ().value ();
    const auto publication_started = started.wait_for (std::chrono::seconds (5));
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    // Release only after observing the bound, so RED also tears down cleanly.
    write.complete (result_t<bool>::success (true));
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, publication_started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (1u, publications.load ());
    EXPECT_FALSE (service.descriptors_published ().result ().value ());
    runtime->stop ();
}

class held_coordinator_read_store_t final : public in_memory_location_repository_t
{
  public:
    using page_t = zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>;
    bool hold_capacity = false;
    zlink::framework::task_completion_source_t<page_t> listed;
    zlink::framework::task_completion_source_t<void> refreshed;
    std::promise<void> entered;
    std::promise<void> second_entered;
    std::atomic<unsigned> reads = 0;
    std::atomic<unsigned> lease_reads = 0;
    std::atomic<unsigned> refreshes = 0;

    zlink::framework::task_t<page_t>
    list_mesh_nodes_core (std::string, zlink::framework::location_page_request_t = {}) override
    {
        const auto round = ++reads;
        if (!hold_capacity) {
            if (round == 1)
                entered.set_value ();
            if (round == 2)
                second_entered.set_value ();
        }
        if (!hold_capacity)
            return listed.task ();
        zlink::framework::mesh_node_descriptor_t peer;
        peer.mesh_name = "ordered-mesh";
        peer.rid = zlink::routing_id_t::from ("snapshot-peer");
        peer.endpoint = "inproc://snapshot-peer";
        peer.owner_id = "snapshot-owner";
        peer.lifecycle_generation = 1;
        peer.descriptor_revision = 1;
        peer.lease_generation = 1;
        peer.state = zlink::framework::framework_runtime_state_t::serving;
        page_t page;
        page.items.push_back (std::move (peer));
        return zlink::framework::task_t<page_t> (
          zlink::framework::result_t<page_t>::success (std::move (page)));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner) override
    {
        using namespace zlink::framework;
        ++lease_reads;
        const auto now = std::chrono::system_clock::now ();
        return task_t<owner_lease_read_result_t> (result_t<owner_lease_read_result_t>::success (
          owner_lease_found_t{{std::move (owner), 1}, now + std::chrono::hours (1), now}));
    }

    zlink::framework::task_t<void> refresh_capacity_metrics (std::string,
                                                             zlink::routing_id_t) override
    {
        const auto round = ++refreshes;
        if (round == 1)
            entered.set_value ();
        if (round == 2)
            second_entered.set_value ();
        return refreshed.task ();
    }

    void release ()
    {
        page_t page;
        zlink::framework::mesh_node_descriptor_t peer;
        peer.mesh_name = "ordered-mesh";
        peer.rid = zlink::routing_id_t::from ("late-peer");
        peer.owner_id = "late-owner";
        peer.lifecycle_generation = 1;
        peer.lease_generation = 1;
        page.items.push_back (std::move (peer));
        listed.complete (zlink::framework::result_t<page_t>::success (std::move (page)));
        refreshed.complete (zlink::framework::result_t<void>::success ());
    }
};

void check_auto_connect_stop_cancels_pending_read (bool hold_capacity, bool expire_first = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_coordinator_read_store_t> ();
    store->hold_capacity = hold_capacity;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("read-stop"));
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    auto reader = std::make_shared<live_location_reader_t> (*store, options);
    services.add_factory<live_location_reader_t> (
      [reader] (service_provider_t &) { return reader; }, service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    std::atomic<unsigned> publications = 0;
    std::atomic<unsigned> connections = 0;
    std::promise<void> published;
    auto publication_entered = published.get_future ();
    std::atomic<bool> tick_failed = false;
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token,
           std::chrono::steady_clock::time_point) -> task_t<bool> {
          tick_failed = location_auto_connect_test_access_t::has_tick_failure (service);
          if (++publications == 1)
              published.set_value ();
          co_return true;
      });
    location_auto_connect_test_access_t::add_test_loop (
      service, [&] (std::string) { ++connections; }, [] {}, hold_capacity);
    auto entered = store->entered.get_future ();
    service.start (provider).result ().value ();
    const auto read_started = entered.wait_for (options.owner_lease_renew_timeout);
    if (expire_first) {
        EXPECT_EQ (std::future_status::ready,
                   publication_entered.wait_for (2 * options.owner_lease_renew_timeout));
        EXPECT_TRUE (tick_failed.load ());
        auto second_read = store->second_entered.get_future ();
        runtime->peer_reconcile_requester () ({});
        EXPECT_EQ (std::future_status::ready,
                   second_read.wait_for (options.owner_lease_renew_timeout));
    }
    const auto before_stop = std::chrono::steady_clock::now ();
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_until (before_stop + options.owner_lease_renew_timeout);
    // The provider stays pending until the stop bound has been measured, including on RED.
    store->release ();
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, read_started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    const auto rounds = expire_first ? 2u : 1u;
    EXPECT_EQ (rounds, store->reads.load ());
    EXPECT_EQ (hold_capacity ? rounds : 0u, store->refreshes.load ());
    EXPECT_EQ (hold_capacity ? rounds : 0u, store->lease_reads.load ());
    EXPECT_EQ (expire_first ? 1u : 0u, publications.load ());
    EXPECT_EQ (hold_capacity && expire_first ? 1u : 0u, connections.load ());
    EXPECT_FALSE (reader->cached_request_source (
      "ordered-mesh", zlink::routing_id_t::from ("late-peer").to_bytes (), 1));
    EXPECT_NO_THROW (failures->rethrow_if_failed ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingListAndIgnoresLateCompletion)
{
    check_auto_connect_stop_cancels_pending_read (false);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingCapacityRefresh)
{
    check_auto_connect_stop_cancels_pending_read (true);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectListTimeoutFailsTickAndAllowsNextPoll)
{
    check_auto_connect_stop_cancels_pending_read (false, true);
}

TEST (ZLinkFrameworkLocationRuntime,
      AutoConnectCapacityTimeoutFailsTickAndReconcilesPolledSnapshotBeforeNextPoll)
{
    check_auto_connect_stop_cancels_pending_read (true, true);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopReportsLoopCompletionTimeout)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    location_runtime_t runtime (*store, options);
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    // Isolate the final wait from Store cancellation; there is no running coroutine.
    auto finished = location_auto_connect_test_access_t::hold_loop_completion (service, runtime);
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (2 * options.owner_lease_renew_timeout);
    finished->complete (result_t<void>::success ());
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_THROW (failures->rethrow_if_failed (), framework_exception_t);
}

class ordered_coordinator_store_t final : public in_memory_location_repository_t
{
  public:
    using write_result = zlink::framework::location_write_result_t;
    zlink::framework::task_completion_source_t<write_result> writes[2];
    zlink::framework::task_completion_source_t<void> entered[2];
    zlink::framework::location_owner_token_t owner;
    std::atomic<unsigned> reads = 0;
    std::atomic<unsigned> publications = 0;
    std::mutex mutex;
    std::vector<std::string> events;

    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        events.push_back (std::move (event));
    }
    std::vector<std::string> observed ()
    {
        std::lock_guard lock (mutex);
        return events;
    }
    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh, zlink::framework::location_page_request_t = {}) override
    {
        using namespace zlink::framework;
        const auto round = ++reads;
        record ("snapshot-" + std::to_string (round));
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = std::move (mesh);
        descriptor.rid = zlink::routing_id_t::from ("peer-" + std::to_string (round));
        descriptor.endpoint = "inproc://peer-" + std::to_string (round);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.state = framework_runtime_state_t::serving;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (descriptor));
        return task_t<decltype (page)> (result_t<decltype (page)>::success (std::move (page)));
    }
    zlink::framework::task_t<write_result>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t,
                      zlink::framework::location_write_intent_t) override
    {
        const auto round = publications++;
        record ("publish-" + std::to_string (round + 1));
        if (round >= 2)
            throw std::logic_error ("unexpected publication after stop");
        entered[round].complete (zlink::framework::result_t<void>::success ());
        return writes[round].task ();
    }
    void release (unsigned round)
    {
        writes[round].complete (zlink::framework::result_t<write_result>::success (
          {zlink::framework::location_write_status_t::stored, round + 1, {}}));
    }
};

void check_auto_connect_snapshot_order_and_late_completion (bool expire_first)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<ordered_coordinator_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout =
      expire_first ? std::chrono::milliseconds (200) : std::chrono::milliseconds (2000);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("ordered-local"));
    store->owner = runtime->current_owner_token ().value ();
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (), {}, handlers,
      serializers, [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [store] (location_write_intent_t intent, std::stop_token cancellation,
               std::chrono::steady_clock::time_point deadline) -> task_t<bool> {
          const auto result =
            co_await observe_until (store->update_mesh_node ({}, intent), deadline, cancellation);
          co_return result &&result.value ().status == location_write_status_t::stored;
      });
    location_auto_connect_test_access_t::add_test_loop (
      service, [store] (std::string rid) { store->record ("connect-" + rid); },
      [store] { store->record ("disconnect"); });
    service.start (provider).result ().value ();
    const auto first = store->entered[0].task ().result_for (std::chrono::seconds (5));
    const auto request = runtime->peer_reconcile_requester ();
    request ({});
    const auto before_completion = store->observed ();
    if (!expire_first)
        store->release (0);
    const auto second = store->entered[1].task ().result_for (std::chrono::seconds (5));
    const auto before_stop = store->observed ();
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    // Finish the provider only after stop's bound has been observed.
    store->release (0);
    store->release (1);
    stopped.get ();
    request ({});
    EXPECT_TRUE (first.has_value ());
    EXPECT_TRUE (second.has_value ());
    EXPECT_EQ ((std::vector<std::string>{"snapshot-1", "publish-1"}), before_completion);
    std::vector<std::string> ordered{"snapshot-1", "publish-1"};
    if (!expire_first)
        ordered.push_back ("connect-peer-1");
    ordered.push_back ("snapshot-2");
    ordered.push_back ("publish-2");
    EXPECT_EQ (ordered, before_stop);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (2u, store->reads.load ());
    EXPECT_EQ (2u, store->publications.load ());
    auto expected = before_stop;
    if (!expire_first)
        expected.push_back ("disconnect");
    EXPECT_EQ (expected, store->observed ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectOrdersSnapshotsAndIgnoresLateStoreCompletion)
{
    check_auto_connect_snapshot_order_and_late_completion (false);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectPublicationTimeoutAllowsNextRenewal)
{
    check_auto_connect_snapshot_order_and_late_completion (true);
}

TEST (ZLinkFrameworkLocationRuntime,
      DescriptorPublicationCancellationAndDeadlineUseExistingObservation)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_descriptor_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (100);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    location_runtime_t runtime (*store, options);
    runtime.start (zlink::routing_id_t::from ("timeout-owner"));
    std::stop_source stopped;
    auto entered = store->entered.get_future ();
    auto publication = runtime.descriptor_write_confirmed (
      [store] { return store->update_mesh_node ({}, location_write_intent_t::new_claim); },
      std::chrono::steady_clock::now, stopped.get_token ());
    EXPECT_EQ (std::future_status::ready, entered.wait_for (std::chrono::seconds (5)));
    const auto timed_out = publication.result_for (std::chrono::seconds (5));
    EXPECT_TRUE (timed_out.has_value ());
    if (timed_out && *timed_out)
        EXPECT_FALSE (timed_out->value ());
    stopped.request_stop ();
    unsigned submissions = 0;
    auto canceled = runtime.descriptor_write_confirmed (
      [&] {
          ++submissions;
          return store->write.task ();
      },
      std::chrono::steady_clock::now, stopped.get_token ());
    EXPECT_FALSE (canceled.result ().value ());
    EXPECT_EQ (0u, submissions);
    store->release ();
    runtime.stop ();
}

class held_domain_descriptor_store_t final : public in_memory_location_repository_t
{
  public:
    using write_result = zlink::framework::location_write_result_t;
    zlink::framework::task_completion_source_t<write_result> write;
    std::promise<void> entered;
    std::atomic<unsigned> publications = 0;
    std::atomic<unsigned> held_writes = 0;
    std::atomic_bool hold_writes = true;
    zlink::framework::task_completion_source_t<void> cleanup_entered;

    zlink::framework::task_t<write_result> pending ()
    {
        ++publications;
        if (!hold_writes.load ())
            return zlink::framework::task_t<write_result> (
              zlink::framework::result_t<write_result>::success (
                {zlink::framework::location_write_status_t::stored, 1, {}}));
        if (++held_writes == 1)
            entered.set_value ();
        else
            cleanup_entered.complete (zlink::framework::result_t<void>::success ());
        return write.task ();
    }
    zlink::framework::task_t<write_result>
    update_client_server (zlink::framework::client_server_server_descriptor_t,
                          zlink::framework::location_write_intent_t) override
    {
        return pending ();
    }
    zlink::framework::task_t<write_result>
    update_fanout_publisher (zlink::framework::fanout_publisher_descriptor_t,
                             zlink::framework::location_write_intent_t) override
    {
        return pending ();
    }
    void release ()
    {
        write.complete (zlink::framework::result_t<write_result>::success (
          {zlink::framework::location_write_status_t::stored, 1, {}}));
    }
};

void check_auto_connect_domain_stop (bool fanout, bool previously_published = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_domain_descriptor_store_t> ();
    store->hold_writes.store (!previously_published);
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::milliseconds (5);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    auto builder = test::runtime_failure_builder ();
    auto channel = builder.channel ("held-domain");
    if (fanout)
        channel.enable_publisher ()
          .set_routing_id (zlink::routing_id_t::from ("held-domain-publisher"))
          .bind ("tcp://127.0.0.1:0");
    else
        channel.enable_server ().bind ("tcp://127.0.0.1:0");
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (),
      detail::channel_runtime_t::from (builder.message_bus ()).channel_snapshots (), handlers,
      serializers, [] { return framework_runtime_state_t::serving; });
    // Resources start without an owner; the first descriptor write belongs to
    // recovery, after the owner lease below becomes usable.
    service.start (provider).result ().value ();
    auto entered = store->entered.get_future ();
    runtime->start (zlink::routing_id_t::from ("held-domain-owner"));
    if (previously_published) {
        const auto published =
          service.descriptors_published ().result_for (std::chrono::seconds (5));
        EXPECT_TRUE (published && *published && published->value ());
        store->hold_writes.store (true);
        runtime->set_draining (true);
        runtime->peer_reconcile_requester () ({});
    }
    const auto started = entered.wait_for (std::chrono::seconds (5));
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    if (previously_published && fanout) {
        // Teardown has its own Store write. Expire that observation with the
        // existing scheduler test clock, without completing the provider.
        const auto cleanup = store->cleanup_entered.task ().result_for (std::chrono::seconds (5));
        EXPECT_TRUE (cleanup.has_value ());
        detail::deadline_scheduler_test_access_t::advance_to (std::chrono::steady_clock::now ()
                                                              + options.owner_lease_renew_timeout);
    }
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    const auto before_late_completion = store->publications.load ();
    store->release ();
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (previously_published ? (fanout ? 3u : 2u) : 1u, before_late_completion);
    EXPECT_EQ (before_late_completion, store->publications.load ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, ClientServerCoordinatorStopCancelsHeldDescriptorWrite)
{
    check_auto_connect_domain_stop (false);
}

TEST (ZLinkFrameworkLocationRuntime, FanoutCoordinatorStopCancelsHeldDescriptorWrite)
{
    check_auto_connect_domain_stop (true);
}

TEST (ZLinkFrameworkLocationRuntime, ClientServerStopAfterPublishedDescriptorIgnoresLateRenewal)
{
    check_auto_connect_domain_stop (false, true);
}

TEST (ZLinkFrameworkLocationRuntime, FanoutStopSharesDeadlineWithDescriptorCleanup)
{
    check_auto_connect_domain_stop (true, true);
}

void check_discovery_reconcile_without_periodic_wait (bool request_during_initial_read)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<requested_peer_store_t> ();
    location_options_t options;
    options.polling_interval = std::chrono::hours (1);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("a-local"));
    store->owner = runtime->current_owner_token ().value ();

    auto builder = test::runtime_failure_builder ();
    builder.route_channel ("wake-mesh").set_routing_id (zlink::routing_id_t::from ("a-local"));
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (builder);
    manager.initialize_route_channels (builder);
    auto &route = manager.get_route_channel ("wake-mesh");
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    std::promise<void> initial_publication;
    auto initially_published = initial_publication.get_future ();
    std::promise<bool> connected;
    auto connection_observed = connected.get_future ();
    auto first_read = store->first_read.get_future ();
    const auto request = runtime->peer_reconcile_requester ();
    unsigned publications = 0;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
        .channel_snapshots (),
      handlers, serializers,
      [&provider] {
          return provider.get_required<zlink::framework::framework_runtime_t> ().status ().state;
      },
      {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token,
           std::chrono::steady_clock::time_point) -> task_t<bool> {
          if (++publications == 1) {
              initial_publication.set_value ();
          } else if (publications == 2) {
              // The callback precedes reconcile. A second request schedules an
              // observation tick after the first requested tick has connected.
              request (requested_peer_rid.to_bytes ());
          } else if (publications == 3) {
              const auto targets = route.list_connection_targets ();
              connected.set_value (targets.size () == 1 && targets.front ().peer_rid
                                   && *targets.front ().peer_rid == requested_peer_rid
                                   && targets.front ().endpoint == "inproc://requested-peer");
          }
          co_return true;
      });
    service.start (provider).result ().value ();
    const auto initial_read_status = first_read.wait_for (heartbeat_observation_timeout);
    store->visible.store (true);
    if (request_during_initial_read)
        request (requested_peer_rid.to_bytes ());
    store->release_first_read.count_down ();
    ASSERT_EQ (std::future_status::ready, initial_read_status);
    ASSERT_EQ (std::future_status::ready,
               initially_published.wait_for (heartbeat_observation_timeout));
    if (!request_during_initial_read) {
        // This bounded observation detects an unsolicited reconcile during the
        // window; it cannot establish that none would occur at a later time.
        EXPECT_EQ (std::future_status::timeout,
                   connection_observed.wait_for (admission_observation_timeout));
        EXPECT_EQ (1u, store->reads.load ());
        EXPECT_TRUE (route.list_connection_targets ().empty ());
        request (requested_peer_rid.to_bytes ());
    }
    const auto observed = connection_observed.wait_for (heartbeat_observation_timeout);
    EXPECT_EQ (std::future_status::ready, observed);
    if (observed == std::future_status::ready)
        EXPECT_TRUE (connection_observed.get ());
    service.stop ();
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, PeerRequestRunsReconcileWithoutPeriodicWait)
{
    check_discovery_reconcile_without_periodic_wait (false);
}

TEST (ZLinkFrameworkLocationRuntime, PeerRequestBeforeWaitRunsReconcileWithoutPeriodicWait)
{
    check_discovery_reconcile_without_periodic_wait (true);
}

class advancing_claim_store_t : public in_memory_location_repository_t
{
  public:
    advancing_claim_store_t (std::chrono::steady_clock::time_point &now,
                             std::chrono::milliseconds claim_duration,
                             bool fail_claim) :
        _now (now), _claim_duration (claim_duration), _fail_claim (fail_claim)
    {
    }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds ttl) override
    {
        _now += _claim_duration;
        if (_fail_claim)
            throw std::runtime_error ("Store unavailable");
        return in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        if (_fail_claim)
            throw std::runtime_error ("Store unavailable");
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }

  private:
    std::chrono::steady_clock::time_point &_now;
    std::chrono::milliseconds _claim_duration;
    bool _fail_claim;
};

TEST (ZLinkFrameworkLocationRuntime, FirstHeartbeatIsDueFromInitialLeaseOperationStart)
{
    using access_t = zlink::framework::runtime::location_runtime_test_access_t;
    for (bool fail_claim : {false, true}) {
        for (auto duration : {std::chrono::milliseconds (50), std::chrono::milliseconds (150)}) {
            SCOPED_TRACE (::testing::Message ()
                          << "fail_claim=" << fail_claim << " claim_ms=" << duration.count ());
            auto now = std::chrono::steady_clock::now ();
            advancing_claim_store_t store (now, duration, fail_claim);
            location_runtime_t runtime (
              store,
              location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (100)},
              "owner-first-heartbeat");
            EXPECT_EQ (access_t::first_heartbeat_delay (runtime, [&] { return now; }),
                       std::max (std::chrono::milliseconds::zero (),
                                 std::chrono::milliseconds (100) - duration));
            EXPECT_TRUE (runtime.cleanup_owner ());
        }
    }
}

TEST (ZLinkFrameworkLocationRuntime, ReceiveAdmissionDoesNotJoinBusyLocationLane)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-admission-snapshot");
    runtime.renew_owner_lease_once ();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    auto &lane = zlink::framework::runtime::location_runtime_test_access_t::lane (runtime);
    ASSERT_TRUE (lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    }));
    entered.get_future ().wait ();
    auto admission = std::async (std::launch::async, [&] {
        const auto token = runtime.current_owner_token ();
        const auto pending = runtime.current_owner_token_task ();
        if (!token || !pending.await_ready ())
            return false;
        const auto task_token = pending.result ().value ();
        return runtime.owner_lease_usable () && task_token.has_value ()
               && task_token->owner_id == token->owner_id
               && task_token->lease_generation == token->lease_generation
               && runtime.owner_lease_healthy () && runtime.owner_lease_renewed_at ().has_value ();
    });
    const auto status = admission.wait_for (admission_observation_timeout);
    release.set_value ();
    EXPECT_EQ (status, std::future_status::ready);
    EXPECT_TRUE (admission.get ());
    EXPECT_TRUE (runtime.cleanup_owner ());
    EXPECT_FALSE (runtime.owner_lease_usable ());
}

TEST (ZLinkFrameworkLocationRuntime, AdmissionSnapshotPreservesStrictExpiryAndRelease)
{
    using access_t = zlink::framework::runtime::location_runtime_test_access_t;
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-admission-expiry");
    EXPECT_FALSE (runtime.owner_lease_usable ());
    runtime.renew_owner_lease_once ();
    auto previous = access_t::snapshot (runtime);
    const auto deadline = std::chrono::steady_clock::now ();
    access_t::set_deadline (runtime, deadline);
    EXPECT_TRUE (access_t::usable_at (runtime, deadline - std::chrono::steady_clock::duration (1)));
    EXPECT_FALSE (access_t::usable_at (runtime, deadline));
    EXPECT_FALSE (
      access_t::usable_at (runtime, deadline + std::chrono::steady_clock::duration (1)));
    EXPECT_FALSE (runtime.owner_lease_usable ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    runtime.renew_owner_lease_once ();
    EXPECT_TRUE (runtime.owner_lease_usable ());
    EXPECT_NE (previous, access_t::snapshot (runtime));
    EXPECT_TRUE (runtime.cleanup_owner ());
    EXPECT_FALSE (runtime.owner_lease_usable ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    EXPECT_TRUE (previous->token.has_value ());
    const auto retained = std::weak_ptr{previous};
    previous.reset ();
    EXPECT_TRUE (retained.expired ());
}

TEST (ZLinkFrameworkLocationRuntime, RejectsLeaseTimeoutLongerThanFencedRenewalWindow)
{
    location_options_t options;
    options.owner_lease_ttl = std::chrono::seconds (15);
    options.owner_lease_renew_interval = std::chrono::seconds (1);
    options.owner_lease_renew_timeout = std::chrono::seconds (8);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_ttl = std::chrono::seconds (30);
    EXPECT_NO_THROW (zlink::framework::detail::validate_location_options (options));
    EXPECT_NO_THROW (zlink::framework::detail::validate_location_options (location_options_t{}));
}

TEST (ZLinkFrameworkLocationRuntime, RejectsLeaseRenewalWindowOverflow)
{
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (1);
    options.owner_lease_renew_timeout =
      std::chrono::milliseconds::max () / 2 + std::chrono::milliseconds (1);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_renew_timeout = std::chrono::milliseconds::max ();
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_renew_interval = std::chrono::milliseconds::max ();
    options.owner_lease_renew_timeout = std::chrono::milliseconds (1);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
}

class timed_renew_repository_t final : public in_memory_location_repository_t
{
  public:
    std::future<std::chrono::steady_clock::time_point> second_renew_called ()
    {
        return _second_renew_called.get_future ();
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t token,
                       std::chrono::milliseconds ttl) override
    {
        if (++_calls == 2)
            _second_renew_called.set_value (std::chrono::steady_clock::now ());
        return in_memory_location_repository_t::renew_owner_lease (std::move (token), ttl);
    }

  private:
    int _calls = 0;
    std::promise<std::chrono::steady_clock::time_point> _second_renew_called;
};

TEST (ZLinkFrameworkLocationRuntime, CompletedHeartbeatWaitsUntilPreviousStartPlusInterval)
{
    timed_renew_repository_t store;
    const auto interval = std::chrono::milliseconds (400);
    location_runtime_t runtime (store,
                                location_options_t{.owner_lease_renew_interval = interval,
                                                   .owner_lease_renew_timeout = interval / 4},
                                "owner-spacing");
    auto second_renew_called = store.second_renew_called ();
    // The runtime starts a renewal before its state lane hands the request to the store, so a
    // store call time is later than the runtime's start time by a scheduling delay that differs
    // per renewal. Only a time taken before startup bounds the runtime's start times from below:
    // the first heartbeat starts one interval after startup, and the second starts one interval
    // after the first started.
    const auto start_requested_at = std::chrono::steady_clock::now ();
    runtime.start (zlink::routing_id_t::from ("node-spacing"));
    ASSERT_EQ (std::future_status::ready, second_renew_called.wait_for (std::chrono::seconds (5)));
    EXPECT_GE (second_renew_called.get () - start_requested_at, 2 * interval);
    runtime.stop ();
}

class slow_claim_repository_t final : public in_memory_location_repository_t
{
  public:
    explicit slow_claim_repository_t (std::chrono::milliseconds claim_delay) :
        _claim_delay (claim_delay)
    {
    }

    std::future<void> first_renew_called () { return _first_renew_called.get_future (); }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds ttl) override
    {
        std::this_thread::sleep_for (_claim_delay);
        return in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t token,
                       std::chrono::milliseconds ttl) override
    {
        if (++_renew_calls == 1)
            _first_renew_called.set_value ();
        return in_memory_location_repository_t::renew_owner_lease (std::move (token), ttl);
    }

  private:
    std::chrono::milliseconds _claim_delay;
    std::atomic_int _renew_calls = 0;
    std::promise<void> _first_renew_called;
};

TEST (ZLinkFrameworkLocationRuntime, RenewLatenessIsMeasuredFromTheHeartbeatSchedule)
{
    // Lateness compares the renewal start with the scheduled deadline (the initial lease
    // operation start plus one interval), rather than with the claim start itself. A slow claim
    // must not be reported in full as heartbeat lateness.
    metric_test::provider_t metrics;
    const auto claim_delay = std::chrono::milliseconds (300);
    slow_claim_repository_t store (claim_delay);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (100),
                         .owner_lease_renew_timeout = std::chrono::seconds (1)},
      "owner-lateness");
    runtime.bind_monitoring (zlink::framework::test::runtime_failure_monitoring ());
    runtime.register_metric_scope ("mesh", "lateness-mesh").result ().value ();
    auto first_renew_called = store.first_renew_called ();
    runtime.start (zlink::routing_id_t::from ("node-lateness"));
    ASSERT_EQ (std::future_status::ready, first_renew_called.wait_for (std::chrono::seconds (5)));

    std::optional<metric_test::sdk::HistogramPointData> lateness;
    const auto data = metrics.collect ();
    metric_test::validate_metric_contract (data);
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != "zlink.location.owner_lease.renew.lateness")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            EXPECT_EQ ("mesh",
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("scope_kind")));
            EXPECT_EQ ("lateness-mesh",
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("scope_name")));
            if (const auto *histogram =
                  opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                    &point.point_data))
                lateness = *histogram;
        }
    }
    runtime.stop ();
    ASSERT_TRUE (lateness.has_value ());
    EXPECT_LT (opentelemetry::nostd::get<double> (lateness->max_),
               std::chrono::duration<double> (claim_delay).count ());
}

class pending_renew_repository_t final : public in_memory_location_repository_t
{
  public:
    std::future<void> renew_started () { return _renew_started.get_future (); }
    std::future<void> next_renew_started () { return _next_renew_started.get_future (); }

    int renew_calls () const noexcept { return _renew_calls.load (); }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t, std::chrono::milliseconds) override
    {
        const auto call = _renew_calls.fetch_add (1);
        if (call != 0) {
            if (call == 1)
                _next_renew_started.set_value ();
            return zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_renew_result_t>::success (
                zlink::framework::owner_lease_renew_result_t{
                  zlink::framework::owner_lease_stale_t{}}));
        }
        _renew_started.set_value ();
        return _renew_completion.task ();
    }

    void complete_renew (zlink::framework::owner_lease_renew_result_t result)
    {
        _renew_completion.complete (
          zlink::framework::result_t<zlink::framework::owner_lease_renew_result_t>::success (
            std::move (result)));
    }

  private:
    std::promise<void> _renew_started;
    std::promise<void> _next_renew_started;
    std::atomic_int _renew_calls = 0;
    zlink::framework::task_completion_source_t<zlink::framework::owner_lease_renew_result_t>
      _renew_completion;
};

TEST (ZLinkFrameworkLocationRuntime, PendingHeartbeatDoesNotOverlapAndCompletionWakesNextRenewal)
{
    pending_renew_repository_t store;
    const auto interval = std::chrono::milliseconds (400);
    location_runtime_t runtime (store, location_options_t{.owner_lease_renew_interval = interval},
                                "owner-pending");
    auto started = store.renew_started ();
    auto next = store.next_renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-pending"));
    ASSERT_EQ (std::future_status::ready, started.wait_for (std::chrono::seconds (5)));
    EXPECT_EQ (std::future_status::timeout, next.wait_for (interval + interval / 4));
    const auto lease = store.read_owner_lease ("owner-pending").result ().value ();
    const auto &found = std::get<zlink::framework::owner_lease_found_t> (lease);
    store.complete_renew (
      zlink::framework::owner_lease_renewed_t{found.lease_expires_at, found.store_now});
    EXPECT_EQ (std::future_status::ready, next.wait_for (interval / 4));
    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, HeartbeatContinuesWhenRenewCompletesAsynchronously)
{
    pending_renew_repository_t store;
    const auto &options = asynchronous_heartbeat_options;
    location_runtime_t runtime (store, options, "owner-a");
    auto renew_started = store.renew_started ();
    const auto observation_deadline =
      std::chrono::steady_clock::now () + options.owner_lease_renew_timeout;
    runtime.start (zlink::routing_id_t::from ("node-a"));
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (heartbeat_observation_timeout));
    const auto lease = store.read_owner_lease ("owner-a").result ().value ();
    const auto &found = std::get<zlink::framework::owner_lease_found_t> (lease);
    const auto renewed_at = found.store_now + std::chrono::seconds (1);
    store.complete_renew (zlink::framework::owner_lease_renewed_t{
      found.lease_expires_at + std::chrono::seconds (1), renewed_at});
    while (store.renew_calls () == 1 && std::chrono::steady_clock::now () < observation_deadline)
        std::this_thread::yield ();
    EXPECT_LT (std::chrono::steady_clock::now (), observation_deadline);
    EXPECT_GT (store.renew_calls (), 1);
    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, StopDiscardsLateHeartbeatRenewCompletion)
{
    pending_renew_repository_t store;
    location_runtime_t runtime (store, asynchronous_heartbeat_options, "owner-a");
    auto renew_started = store.renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    const auto initial_renewed_at = runtime.owner_lease_renewed_at ();
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (heartbeat_observation_timeout));

    runtime.stop ();
    const auto late_store_now = *initial_renewed_at + std::chrono::seconds (1);
    store.complete_renew (zlink::framework::owner_lease_renewed_t{
      late_store_now + std::chrono::seconds (15), late_store_now});
    EXPECT_EQ (initial_renewed_at, runtime.owner_lease_renewed_at ());
}

TEST (ZLinkFrameworkLocationRuntime, HostShutdownDiscardsLateHeartbeatStoreCompletion)
{
    using namespace zlink::framework;
    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    pending_renew_repository_t store;
    location_runtime_t location (store, asynchronous_heartbeat_options, "owner-late-host");
    auto started = store.renew_started ();
    location.start (zlink::routing_id_t::from ("node-late-host"));
    EXPECT_EQ (std::future_status::ready, started.wait_for (heartbeat_observation_timeout));
    const auto renewed_at = location.owner_lease_renewed_at ();
    location.stop ();
    runtime::shutdown_handler_coroutine_executor ();
    EXPECT_NO_THROW (store.complete_renew (owner_lease_stale_t{}));
    EXPECT_EQ (renewed_at, location.owner_lease_renewed_at ());
}

class startup_owner_lease_repository_t final : public in_memory_location_repository_t
{
  public:
    enum class claim_mode_t
    {
        fail_first,
        hold_first,
        commit_then_fail,
        never_complete,
        generation_exhausted,
        succeed
    };

    ~startup_owner_lease_repository_t () override
    {
        if (_delayed_claim.joinable ())
            _delayed_claim.join ();
    }

    void set_claim_mode (claim_mode_t mode) { _claim_mode = mode; }

    void hold_first_claim () { _claim_mode = claim_mode_t::hold_first; }

    // Commits the held first claim from another thread, as a late provider reply would.
    void release_first_claim ()
    {
        _delayed_claim = std::thread ([this] {
            auto committed = in_memory_location_repository_t::claim_owner_lease (
              std::move (_held_owner_id), _held_lease_ttl);
            _held_claim->complete (committed.result ());
        });
    }

    void cancel_after_next_claim (std::stop_source &cancellation) { _cancellation = &cancellation; }

    void fail_owner_lease_reads () noexcept { _fail_reads = true; }

    void fail_owner_lease_releases () noexcept { _fail_releases = true; }

    void block_owner_lease_releases ()
    {
        _block_releases = true;
        _pending_release = std::make_shared<zlink::framework::task_completion_source_t<
          zlink::framework::owner_lease_release_result_t>> ();
    }

    auto release_started () { return _release_started.task (); }

    void complete_pending_release ()
    {
        _pending_release->complete (
          zlink::framework::result_t<zlink::framework::owner_lease_release_result_t>::success (
            zlink::framework::owner_lease_release_result_t{
              zlink::framework::owner_lease_released_t{}}));
    }

    int claim_calls () const noexcept { return _claim_calls.load (); }

    int release_calls () const noexcept { return _release_calls.load (); }

    int read_calls () const noexcept { return _read_calls.load (); }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        _read_calls.fetch_add (1);
        if (_fail_reads) {
            return zlink::framework::task_t<zlink::framework::owner_lease_read_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease confirmation read failure"));
        }
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override
    {
        const auto call = _claim_calls.fetch_add (1);
        if (_claim_mode == claim_mode_t::fail_first && call == 0) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease provider failure"));
        }
        if (_claim_mode == claim_mode_t::hold_first && call == 0) {
            _held_claim = std::make_shared<zlink::framework::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            _held_owner_id = std::move (owner_id);
            _held_lease_ttl = lease_ttl;
            return _held_claim->task ();
        }
        if (_claim_mode == claim_mode_t::commit_then_fail) {
            in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), lease_ttl)
              .result ()
              .value ();
            if (_cancellation != nullptr) {
                _cancellation->request_stop ();
                _cancellation = nullptr;
            }
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease response failure"));
        }
        if (_claim_mode == claim_mode_t::never_complete) {
            _pending_claim = std::make_shared<zlink::framework::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            if (_cancellation != nullptr) {
                _cancellation->request_stop ();
                _cancellation = nullptr;
            }
            return _pending_claim->task ();
        }
        if (_claim_mode == claim_mode_t::generation_exhausted) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::success (
                zlink::framework::owner_lease_claim_result_t{
                  zlink::framework::owner_lease_generation_exhausted_t{}}));
        }
        auto result =
          in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), lease_ttl);
        if (_cancellation != nullptr) {
            _cancellation->request_stop ();
            _cancellation = nullptr;
        }
        return result;
    }

    zlink::framework::task_t<zlink::framework::owner_lease_release_result_t>
    release_owner_lease (zlink::framework::location_owner_token_t token) override
    {
        _release_calls.fetch_add (1);
        _release_started.complete (zlink::framework::result_t<void>::success ());
        if (_block_releases) {
            return _pending_release->task ();
        }
        if (_fail_releases) {
            return zlink::framework::task_t<zlink::framework::owner_lease_release_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_release_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease release failure"));
        }
        return in_memory_location_repository_t::release_owner_lease (std::move (token));
    }

  private:
    claim_mode_t _claim_mode = claim_mode_t::fail_first;
    std::stop_source *_cancellation = nullptr;
    std::thread _delayed_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_claim_result_t>>
      _held_claim;
    std::string _held_owner_id;
    std::chrono::milliseconds _held_lease_ttl{0};
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_claim_result_t>>
      _pending_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_release_result_t>>
      _pending_release;
    zlink::framework::task_completion_source_t<void> _release_started;
    std::atomic_int _claim_calls = 0;
    std::atomic_int _release_calls = 0;
    std::atomic_int _read_calls = 0;
    bool _fail_reads = false;
    bool _fail_releases = false;
    bool _block_releases = false;
};

TEST (ZLinkFrameworkLocationRuntime, ClaimsAndReleasesOwnerLease)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    runtime.start (zlink::routing_id_t::from ("node-a"));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_found_t> (
      store.read_owner_lease ("owner-a").result ().value ()));

    runtime.stop ();
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_missing_t> (
      store.read_owner_lease ("owner-a").result ().value ()));
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeOwnerCleanup)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    const auto cleanup_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (store, location_options_t{}, "owner-a");
    // Prepare the lease with the normal startup deadline, without a heartbeat.
    // The cleanup under test keeps its separate 20ms absolute deadline.
    runtime.renew_owner_lease_once ();
    ASSERT_TRUE (runtime.current_owner_token ().has_value ());
    auto release_started = store.release_started ();

    auto cleanup = std::async (std::launch::async, [&] {
        return runtime.cleanup_owner (std::chrono::steady_clock::now () + cleanup_timeout);
    });
    const auto started = release_started.result_for (std::chrono::seconds (1));
    if (!started) {
        store.complete_pending_release ();
        EXPECT_FALSE (cleanup.get ());
        FAIL () << "owner lease release did not start";
    }
    ASSERT_TRUE (*started);
    const auto status = cleanup.wait_for (cleanup_timeout + std::chrono::seconds (1));
    EXPECT_EQ (std::future_status::timeout, status);
    store.complete_pending_release ();
    EXPECT_FALSE (cleanup.get ());
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_NE (std::string::npos, runtime.last_error ()->find ("timed out"));
}

TEST (ZLinkFrameworkLocationRuntime, HostCleanupUsesShutdownDeadline)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    auto runtime = std::make_unique<location_runtime_t> (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::seconds (2)},
      "owner-a");
    runtime->start (zlink::routing_id_t::from ("node-a"));
    auto *location = runtime.get ();
    auto host = zlink::framework::app_t::create ();
    host.advanced ().services ().add_singleton (std::move (runtime));
    const auto deadline = std::chrono::milliseconds (200);

    auto shutdown = host.shutdown (deadline);
    auto completed = zlink::framework::detail::observe_task_result_for (
      shutdown, deadline + std::chrono::seconds (1), std::stop_token{});
    EXPECT_FALSE (completed.has_value ());
    store.complete_pending_release ();
    completed = zlink::framework::detail::observe_task_result_for (
      shutdown, std::chrono::seconds (1), std::stop_token{});
    ASSERT_TRUE (completed.has_value ());
    ASSERT_TRUE (completed->has_value ());
    EXPECT_EQ (zlink::framework::termination_outcome_t::force_stopped, completed->value ().outcome);
    EXPECT_EQ (zlink::framework::termination_reason_t::deadline_exceeded,
               completed->value ().reason);
    EXPECT_EQ (1, store.release_calls ());
    ASSERT_TRUE (location->last_error ().has_value ());
    EXPECT_NE (std::string::npos, location->last_error ()->find ("timed out"));
}

TEST (ZLinkFrameworkLocationRuntime, HostCleanupPreservesProviderFailure)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.fail_owner_lease_releases ();
    auto runtime = std::make_unique<location_runtime_t> (store, location_options_t{}, "owner-a");
    runtime->start (zlink::routing_id_t::from ("node-a"));
    auto host = zlink::framework::app_t::create ();
    host.advanced ().services ().add_singleton (std::move (runtime));

    const auto completed = host.shutdown (std::chrono::seconds (1)).result ().value ();
    EXPECT_EQ (zlink::framework::termination_outcome_t::force_stopped, completed.outcome);
    EXPECT_EQ (zlink::framework::termination_reason_t::teardown_failed, completed.reason);
    EXPECT_EQ (1, store.release_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, CleanupStopsPendingHeartbeatWithinDeadline)
{
    pending_renew_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");
    auto renew_started = store.renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (std::chrono::seconds (5)));
    const auto started_at = std::chrono::steady_clock::now ();
    const auto deadline = std::chrono::milliseconds (20);

    EXPECT_TRUE (runtime.cleanup_owner (started_at + deadline));
    EXPECT_LT (std::chrono::steady_clock::now () - started_at, deadline + std::chrono::seconds (1));
    store.complete_renew (zlink::framework::owner_lease_stale_t{});
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, StartsDegradedAfterInitialOwnerLeaseClaimFailure)
{
    startup_owner_lease_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    for (int attempt = 0; attempt != 50 && !runtime.current_owner_token (); ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (runtime.current_owner_token ().has_value ());
    EXPECT_GE (store.claim_calls (), 2);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, RecordsInitialClaimConfirmationReadFailure)
{
    startup_owner_lease_repository_t store;
    store.fail_owner_lease_reads ();
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_EQ ("injected owner lease confirmation read failure", *runtime.last_error ());
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, InstallsLateCommittedClaimAfterHeartbeatConflict)
{
    startup_owner_lease_repository_t store;
    store.hold_first_claim ();
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (40),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    // The first claim stays pending, so start() returns only through the renew timeout.
    runtime.start (zlink::routing_id_t::from ("node-a"));

    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    store.release_first_claim ();
    for (int attempt = 0; attempt != 250 && !runtime.current_owner_token (); ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_EQ (0, store.release_calls ());
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    ASSERT_TRUE (runtime.current_owner_token ().has_value ());
    EXPECT_EQ (1, runtime.current_owner_token ()->lease_generation);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimConflict)
{
    in_memory_location_repository_t store;
    const auto existing =
      store.claim_owner_lease ("owner-a", std::chrono::seconds (15)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<zlink::framework::owner_lease_claimed_t> (existing));
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimGenerationExhaustion)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::generation_exhausted);
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, ReleasesCommittedClaimWhenStartupIsCancelled)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeClaimAndPreservesCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::never_complete);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_LT (std::chrono::steady_clock::now () - started_at, renew_timeout);
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, RecordsCleanupFailureAndPreservesCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.fail_owner_lease_releases ();
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::milliseconds (20)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_EQ ("injected owner lease release failure", *runtime.last_error ());
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeCancellationRelease)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_LT (std::chrono::steady_clock::now () - started_at,
               renew_timeout + std::chrono::milliseconds (60));
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, ReleasesLateCommitWithoutInstalledTokenOnCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::commit_then_fail);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::milliseconds (20)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    const auto remaining = store.read_owner_lease ("owner-a").result ().value ();
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_missing_t> (remaining));
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, DoesNotReadAfterClaimConsumesRenewDeadline)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::never_complete);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    EXPECT_GE (std::chrono::steady_clock::now () - started_at, renew_timeout);
    EXPECT_EQ (0, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    runtime.stop ();
}

} // namespace

TEST (ZLinkFrameworkLocationRuntime, ForceStop2646ObservesPendingCleanupBeforeReturn)
{
    using namespace zlink::framework;
    class pending_cleanup_t final : public in_memory_location_repository_t
    {
      public:
        task_completion_source_t<std::int64_t> completion;
        std::promise<void> entered;
        std::atomic<int> releases{0};
        task_t<std::int64_t> remove_all_by_owner (location_owner_token_t) override
        {
            entered.set_value ();
            return completion.task ();
        }
        task_t<owner_lease_release_result_t>
        release_owner_lease (location_owner_token_t owner) override
        {
            ++releases;
            return in_memory_location_repository_t::release_owner_lease (std::move (owner));
        }
    } store;
    location_runtime_t runtime (store, {}, "cleanup-owner");
    runtime.renew_owner_lease_once ();
    auto entered = store.entered.get_future ();
    EXPECT_FALSE (runtime.cleanup_owner (std::chrono::steady_clock::now ()));
    EXPECT_EQ (std::future_status::timeout, entered.wait_for (std::chrono::milliseconds (0)));
    EXPECT_EQ (0, store.releases.load ());
    auto stopping = std::async (std::launch::async, [&] {
        return runtime.cleanup_owner (std::chrono::steady_clock::now ()
                                      + std::chrono::milliseconds (20));
    });
    EXPECT_EQ (std::future_status::ready, entered.wait_for (std::chrono::seconds (1)));
    EXPECT_EQ (std::future_status::timeout, stopping.wait_for (std::chrono::milliseconds (40)));
    store.completion.complete (result_t<std::int64_t>::success (0));
    EXPECT_FALSE (stopping.get ());
    EXPECT_EQ (0, store.releases.load ());
}


namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;

template <typename Cache> task_t<void> recover_auto_cache (Cache *cache)
{
    if constexpr (requires { cache->invalidate_all_routes_after_store_recovery_async (); })
        co_await cache->invalidate_all_routes_after_store_recovery_async ();
    else
        cache->invalidate_all_routes_after_store_recovery ();
    co_return;
}

template <typename Raw> task_t<void> raw_auto_update (Raw *raw, int operation)
{
    const auto rid = zlink::routing_id_t::from ("remote");
    mesh::service_node_descriptor_t descriptor;
    descriptor.node_routing_id = rid.to_bytes ();
    descriptor.advertised_endpoint = "inproc://l4-peer";
    if constexpr (requires { raw->expect_peer_async (descriptor); }) {
        if (operation == 0)
            co_await raw->connect_peer_intent_async ("inproc://l4-peer", rid, 1, "default", true);
        else if (operation == 1)
            co_await raw->disconnect_peer_intent_async (rid, "inproc://l4-peer", true);
        else if (operation == 2)
            co_await raw->expect_peer_async (descriptor);
        else
            co_await raw->forget_peer_async (rid.to_bytes (), "inproc://l4-peer");
    } else {
        if (operation == 0)
            raw->connect_peer_intent ("inproc://l4-peer", rid, 1, "default", true);
        else if (operation == 1)
            raw->disconnect_peer_intent (rid, "inproc://l4-peer", true);
        else if (operation == 2)
            raw->expect_peer (descriptor);
        else
            raw->forget_peer (rid.to_bytes (), "inproc://l4-peer");
    }
    co_return;
}

template <typename Node> task_t<void> auto_mesh_name (Node *node)
{
    if constexpr (requires {
                      node->connect_peer_async (zlink::routing_id_t::from ("remote"), std::string{},
                                                1, std::string{}, true);
                  }) {
        const auto identity = co_await node->identity_async ();
        EXPECT_EQ ("l4-mesh", identity.first);
    } else {
        EXPECT_EQ ("l4-mesh", node->mesh_name ());
    }
    co_return;
}

// Occupy the owner from an external caller, leaving exactly one infrastructure
// worker available. Submission must return a pending task and free that worker.
void check_auto_owner_wait (state_lane_t &lane, std::function<task_t<void> ()> work)
{
    auto executor = infrastructure_executor ();
    auto workers = test::reserve_single_infrastructure_worker (*executor);
    std::promise<void> entered, release, returned, probe;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();
    auto terminal = std::make_shared<task_completion_source_t<void>> ();
    executor->submit ([&, terminal] {
        auto pending = work ();
        EXPECT_FALSE (pending.await_ready ());
        detail::observe_task_terminal (
          pending, [terminal] (const result_t<void> &result) { terminal->complete (result); });
        returned.set_value ();
    });
    auto return_status = returned.get_future ().wait_for (auto_connect_wait);
    executor->submit ([&] { probe.set_value (); });
    auto probe_status = probe.get_future ().wait_for (auto_connect_wait);
    release.set_value ();
    held.get ();
    EXPECT_EQ (std::future_status::ready, return_status);
    EXPECT_EQ (std::future_status::ready, probe_status);
    auto result = terminal->task ().result_for (auto_connect_wait);
    ASSERT_TRUE (result);
    EXPECT_TRUE (*result);
}

struct production_auto_fixture_t
{
    decltype (test::runtime_failure_builder ()) builder{test::runtime_failure_builder ()};
    handler_registry_t handlers;
    serializer_registry_t serializers;
    std::shared_ptr<runtime_failure_collector_t> failures =
      std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service;

    explicit production_auto_fixture_t (
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes = {}) :
        service (
          failures,
          builder.message_bus (),
          {},
          handlers,
          serializers,
          [] { return framework_runtime_state_t::serving; },
          {},
          {},
          {},
          std::move (nodes))
    {
    }
};

void check_auto_route (bool remove)
{
    detail::route_channel_runtime_t route ("l4-mesh");
    if (remove)
        ASSERT_TRUE (route.connect (zlink::routing_id_t::from ("remote"), "inproc://l4-peer"));
    production_auto_fixture_t fixture;
    check_auto_owner_wait (auto_connect_owner_lane (route), [&] {
        return location_auto_connect_test_access_t::route_target (fixture.service, route, remove);
    });
    EXPECT_EQ (remove ? 0u : 1u, route.list_connections ().size ());
}

void check_auto_raw (int operation)
{
    mesh::raw_mesh_node_options_t options;
    options.descriptor = {"l4-mesh", zlink::routing_id_t::from ("local").to_bytes (), 1, 1,
                          "inproc://l4-local"};
    mesh::raw_mesh_node_owner_t raw (test::runtime_failure_options (std::move (options)));
    check_auto_owner_wait (auto_connect_owner_lane (raw),
                           [&] { return raw_auto_update (&raw, operation); });
}
void check_production_identity ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    check_auto_owner_wait (state->lane, [&] {
        return location_auto_connect_test_access_t::route_target (fixture.service, route, false);
    });
}

void check_mesh_and_host_async_wrappers ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    auto &host = node->native_node ();
    auto &raw_lane = auto_connect_owner_lane (host.transport ());
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-peer";
    const std::pair<const char *, std::function<task_t<void> ()>> wrappers[] = {
      {"mesh.connect_peer_async",
       [&] { return node->connect_peer_async (rid, endpoint, 1, "default", true); }},
      {"mesh.expect_peer_async",
       [&] { return node->expect_peer_async (rid, endpoint, 1, "default"); }},
      {"mesh.forget_peer_async", [&] { return node->forget_peer_async (rid, endpoint); }},
      {"mesh.disconnect_peer_async.raw",
       [&] { return node->disconnect_peer_async (rid, endpoint, true); }},
      {"host.expect_peer_async",
       [&] { return host.expect_peer_async (endpoint, rid, 1, "default"); }},
      {"host.forget_peer_async", [&] { return host.forget_peer_async (endpoint, rid); }}};
    for (const auto &[name, work] : wrappers) {
        SCOPED_TRACE (name);
        check_auto_owner_wait (raw_lane, work);
    }
    check_auto_owner_wait (state->lane,
                           [&] { return node->disconnect_peer_async (rid, endpoint, true); });
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    for (bool remove : {false, true}) {
        SCOPED_TRACE (remove);
        check_auto_owner_wait (raw_lane, [&] {
            return location_auto_connect_test_access_t::route_target (fixture.service, route,
                                                                      remove);
        });
        check_auto_owner_wait (raw_lane, [&] {
            return location_auto_connect_test_access_t::mesh_target (node, remove);
        });
    }
    node->stop ();
}

class ready_auto_store_t : public in_memory_location_repository_t
{
  public:
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string, location_page_request_t) override
    {
        mesh_node_descriptor_t peer;
        peer.mesh_name = "l4-mesh";
        peer.rid = zlink::routing_id_t::from ("remote");
        peer.endpoint = "inproc://l4-peer";
        peer.owner_id = "remote-owner";
        peer.lease_generation = 1;
        peer.lifecycle_generation = 1;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (peer));
        return task_t<location_page_t<mesh_node_descriptor_t>> (
          result_t<location_page_t<mesh_node_descriptor_t>>::success (std::move (page)));
    }
    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner) override
    {
        const auto now = std::chrono::system_clock::now ();
        return task_t<owner_lease_read_result_t> (result_t<owner_lease_read_result_t>::success (
          owner_lease_found_t{{std::move (owner), 1}, now + std::chrono::hours (3), now}));
    }
};

void check_auto_loop_ready (bool later, bool recovery)
{
    ready_auto_store_t store;
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    location_runtime_t runtime (store, options, "local-owner");
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    runtime.start (zlink::routing_id_t::from ("local"));
    live_location_reader_t reader (store, options);
    store_location_resolvers_t cache (store);
    if (recovery)
        auto_connect_cache_test_access_t::seed (cache);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {});
    unsigned calls = 0;
    location_auto_connect_test_access_t::configure_probe (
      service, runtime, store, reader, route, calls, later, recovery ? &cache : nullptr);
    check_auto_owner_wait (
      recovery ? auto_connect_cache_test_access_t::lane (cache) : auto_connect_owner_lane (route),
      [&] { return location_auto_connect_test_access_t::run_probe (service); });
    EXPECT_EQ (later ? 2u : 1u, calls);
    if (recovery)
        auto_connect_cache_test_access_t::expect_recovered (cache);
    runtime.stop ();
    failures->rethrow_if_failed ();
}

void check_provider_auto_loop (bool hold_route)
{
    auto executor = infrastructure_executor ();
    ASSERT_EQ (1u, executor->live_worker_count ());
    auto_connect_probe_store_t provider;
    provider_location_repository_t store (provider);
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    const auto claim =
      store.claim_owner_lease ("remote-owner", options.owner_lease_ttl).result ().value ();
    const auto token = std::get<owner_lease_claimed_t> (claim).token;
    mesh_node_descriptor_t peer;
    peer.mesh_name = "l4-mesh";
    peer.rid = zlink::routing_id_t::from ("remote");
    peer.endpoint = "inproc://l4-peer";
    peer.owner_id = token.owner_id;
    peer.lease_generation = token.lease_generation;
    peer.lifecycle_generation = 1;
    peer.descriptor_revision = 1;
    peer.state = framework_runtime_state_t::serving;
    ASSERT_EQ (
      location_write_status_t::stored,
      store.update_mesh_node (peer, location_write_intent_t::new_claim).result ().value ().status);
    location_runtime_t runtime (store, options, "local-owner");
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    runtime.start (zlink::routing_id_t::from ("local"));
    live_location_reader_t reader (store, options);
    store_location_resolvers_t cache (store);
    auto_connect_cache_test_access_t::seed (cache);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    location_auto_connect_test_access_t::provider_probe_t probe{provider, store, runtime, route,
                                                                peer};
    location_auto_connect_test_access_t::configure_provider_probe (service, probe, reader, cache);
    if (hold_route) {
        check_auto_owner_wait (auto_connect_owner_lane (route), [&] {
            return location_auto_connect_test_access_t::run_probe (service);
        });
    } else {
        auto terminal = std::make_shared<task_completion_source_t<void>> ();
        executor->submit ([&service, terminal] {
            auto pending = location_auto_connect_test_access_t::run_probe (service);
            detail::observe_task_terminal (
              pending, [terminal] (const result_t<void> &result) { terminal->complete (result); });
        });
        const auto result = terminal->task ().result_for (auto_connect_wait);
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
    }
    EXPECT_EQ (2u, probe.connects);
    EXPECT_EQ (1u, probe.disconnects);
    EXPECT_EQ (5u, probe.ticks);
    EXPECT_TRUE (probe.later_wait_ready);
    EXPECT_TRUE (probe.saw_failure);
    EXPECT_FALSE (location_auto_connect_test_access_t::has_tick_failure (service));
    EXPECT_EQ (1u, provider.failures);
    EXPECT_GE (provider.scans, 4u);
    EXPECT_GT (provider.reads, 0u);
    EXPECT_GT (provider.writes, 0u);
    EXPECT_EQ (1u, route.list_connections ().size ());
    auto_connect_cache_test_access_t::expect_recovered (cache);
    service.stop ();
    EXPECT_TRUE (route.list_connections ().empty ());
    runtime.stop ();
    failures->rethrow_if_failed ();
}

template <typename Node> task_t<void> auto_manual_disconnect (Node *node, zlink::routing_id_t rid)
{
    if constexpr (requires { node->disconnect_peer_async (rid, std::string{}, true); })
        co_await node->disconnect_peer_async (rid, "inproc://l4-manual", true);
    else
        node->disconnect_peer (rid, "inproc://l4-manual", true);
    co_return;
}

void check_auto_manual ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    const auto rid = zlink::routing_id_t::from ("remote");
    state->peer_connections.push_back (
      {.expected_routing_id = rid, .endpoint = "inproc://l4-manual"});
    detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    auto &topology = node.native_node ().transport ().topology ();
    auto peer = topology.local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-manual";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (peer, {1}));
    // Manual endpoint remains configured even when discovery forgets its descriptor.
    check_auto_owner_wait (state->lane, [&] { return auto_manual_disconnect (&node, rid); });
    EXPECT_NE (nullptr, topology.peer (rid.to_bytes ()));
    EXPECT_EQ (1u, state->peer_connections.size ());
    EXPECT_EQ ("inproc://l4-manual", state->peer_connections.front ().endpoint);
    node.stop ();
}

task_t<void> disconnect_and_check_notification (mesh::raw_mesh_node_owner_t *raw, unsigned *changes)
{
    co_await raw_auto_update (raw, 1);
    EXPECT_EQ (1u, *changes);
}

void check_auto_notifications ()
{
    mesh::raw_mesh_node_options_t options;
    options.descriptor = {"l4-mesh", zlink::routing_id_t::from ("local").to_bytes (), 1, 1,
                          "inproc://l4-local"};
    mesh::raw_mesh_node_owner_t raw (test::runtime_failure_options (std::move (options)));
    raw.start ();
    const auto rid = zlink::routing_id_t::from ("remote");
    raw.connect_peer_intent ("inproc://l4-peer", rid, 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-peer";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    unsigned changes = 0;
    raw.topology ().set_change_handler ([&] {
        EXPECT_EQ (nullptr, state_lane_t::current ());
        EXPECT_TRUE (raw.topology ().peers ().empty ());
        ++changes;
    });
    check_auto_owner_wait (auto_connect_owner_lane (raw),
                           [&] { return disconnect_and_check_notification (&raw, &changes); });
    EXPECT_EQ (1u, changes);
    EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    raw.topology ().set_change_handler ({});
    raw.close ();
}

void check_auto_stale_peer ()
{
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto old_rid = zlink::routing_id_t::from ("old");
    const auto replacement = zlink::routing_id_t::from ("remote");
    ASSERT_TRUE (route.connect (replacement, "inproc://l4-peer"));
    const auto removed = route.disconnect_async (old_rid, "inproc://l4-peer").result ().value ();
    EXPECT_FALSE (removed);
    const auto targets = route.list_connection_targets ();
    ASSERT_EQ (1u, targets.size ());
    EXPECT_EQ (replacement, targets.front ().peer_rid);
}

}

TEST (AutoConnectLaneDeathTest, OneWorkerProviderBackedStoreLoop)
{
#if defined(__linux__)
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      {
          // The process affinity is the existing shared-pool size input. A fresh
          // death-test child has no pool yet, so this creates exactly one worker.
          cpu_set_t available;
          CPU_ZERO (&available);
          if (sched_getaffinity (getpid (), sizeof (available), &available) != 0)
              std::_Exit (2);
          int cpu = 0;
          while (cpu < CPU_SETSIZE && !CPU_ISSET (cpu, &available))
              ++cpu;
          cpu_set_t single;
          CPU_ZERO (&single);
          CPU_SET (cpu, &single);
          if (sched_setaffinity (getpid (), sizeof (single), &single) != 0)
              std::_Exit (2);
          check_provider_auto_loop (false);
          check_provider_auto_loop (true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
#else
    GTEST_SKIP () << "One-worker probe uses the Linux process-affinity pool-size input";
#endif
}

TEST (AutoConnectLaneDeathTest, ProductionRouteCallbackReleasesIdentityWorker)
{
    ASSERT_EXIT (
      {
          check_production_identity ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, MeshAndHostAsyncWrappersReleaseWorker)
{
    ASSERT_EXIT (
      {
          check_mesh_and_host_async_wrappers ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, RouteConnectionAddReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_route (false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RouteConnectionRemoveReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_route (true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RecoveryCacheReleasesWorker)
{
    ASSERT_EXIT (
      {
          in_memory_location_repository_t store;
          store_location_resolvers_t cache (store);
          check_auto_owner_wait (auto_connect_cache_test_access_t::lane (cache),
                                 [&] { return recover_auto_cache (&cache); });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, MeshIdentityReleasesWorker)
{
    ASSERT_EXIT (
      {
          auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
          detail::mesh_node_runtime_t node (state);
          check_auto_owner_wait (state->lane, [&] { return auto_mesh_name (&node); });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawConnectIntentReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (0);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawDisconnectIntentReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (1);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawExpectationReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (2);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawForgetReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (3);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, FirstIterationAllReadyReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (false, false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, LaterReadyLeaseChangeReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (true, false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RecoveryIterationReleasesCacheWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (false, true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, ManualRouteSnapshotReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_manual ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, DisconnectNotifiesOutsideOwnerBeforeTaskCompletion)
{
    ASSERT_EXIT (
      {
          check_auto_notifications ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, StaleRouteDisconnectRetainsReplacement)
{
    ASSERT_EXIT (
      {
          check_auto_stale_peer ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

namespace
{
// A contended synchronous wrapper must return after its owner turn even while
// every completion-dispatcher worker is occupied. Queue admission orders release.
void check_sync_owner_wait (const char *name, state_lane_t &lane, std::function<void ()> work)
{
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();

    completion_owner_t completion_owner;
    auto dispatcher = detail::borrow_completion_dispatcher ();
    const auto workers = detail::completion_dispatcher_worker_count ();
    std::latch occupied (workers), release_dispatcher (1), dispatched (workers);
    for (std::size_t index = 0; index < workers; ++index)
        detail::dispatch_completion ([&, dispatcher] {
            occupied.count_down ();
            release_dispatcher.wait ();
            dispatched.count_down ();
        });
    occupied.wait ();

    auto invocation = std::async (std::launch::async, std::move (work));
    const auto deadline = std::chrono::steady_clock::now () + auto_connect_wait;
    while (state_lane_test_access_t::pending_work (lane) == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool queued = state_lane_test_access_t::pending_work (lane) != 0;
    release.set_value ();
    held.get ();
    const auto returned = invocation.wait_for (auto_connect_wait);
    release_dispatcher.count_down ();
    dispatched.wait ();
    invocation.get ();
    if (!queued || returned != std::future_status::ready)
        std::fprintf (stderr, "%s: queued=%d returned_without_dispatcher=%d\n", name,
                      static_cast<int> (queued),
                      static_cast<int> (returned == std::future_status::ready));
    EXPECT_TRUE (queued);
    EXPECT_EQ (std::future_status::ready, returned);
}

void check_synchronous_stop (bool mesh, bool raw_owner)
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto rid = zlink::routing_id_t::from ("remote");
    route.connect (rid, "inproc://l4-peer");
    auto &raw = node->native_node ().transport ();
    node->connect_peer (rid, "inproc://l4-peer", 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-peer";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    ASSERT_NE (nullptr, raw.topology ().peer (rid.to_bytes ()));
    location_auto_connect_test_access_t::register_stop_probe (fixture.service, route, node, mesh);
    auto &lane = raw_owner ? auto_connect_owner_lane (node->native_node ().transport ())
                           : (mesh ? state->lane : auto_connect_owner_lane (route));
    check_sync_owner_wait ("auto-connect.stop", lane, [&] { fixture.service.stop (); });
    if (!mesh)
        EXPECT_TRUE (route.list_connections ().empty ());
    else
        EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    fixture.failures->rethrow_if_failed ();
    node->stop ();
}

// Keep every infrastructure worker occupied until the synchronous caller
// returns. A contended lane must grant that caller its turn rather than run
// the disconnect on the thread releasing the predecessor's turn.
void check_mesh_stop_without_infrastructure (int operation)
{
    auto executor = infrastructure_executor ();
    auto reserved = test::reserve_single_infrastructure_worker (*executor);
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    auto &raw = node->native_node ().transport ();
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-peer";
    node->expect_peer (rid, endpoint, 1, "default");
    node->connect_peer (rid, endpoint, 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = endpoint;
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    location_auto_connect_test_access_t::register_stop_probe (fixture.service, route, node, true);
    auto &lane = operation == 0 ? state->lane : auto_connect_owner_lane (raw);
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();
    std::latch occupied (1), release_executor (1), returned_executor (1);
    executor->submit ([&] {
        occupied.count_down ();
        release_executor.wait ();
        returned_executor.count_down ();
    });
    occupied.wait ();
    auto invocation = std::async (std::launch::async, [&] {
        if (operation < 2)
            fixture.service.stop ();
        else if (operation == 2)
            raw.disconnect_peer_intent (endpoint);
        else if (operation == 3)
            raw.disconnect_peer_intent (rid, endpoint, true);
        else
            raw.forget_peer (rid.to_bytes (), endpoint);
    });
    const auto deadline = std::chrono::steady_clock::now () + auto_connect_wait;
    while (state_lane_test_access_t::pending_work (lane) == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    EXPECT_GT (state_lane_test_access_t::pending_work (lane), 0u);
    const bool caller_turn = state_lane_test_access_t::pending_synchronous_turn (lane);
    if (!caller_turn)
        std::fprintf (stderr, "mesh stop operation=%d did not grant the synchronous caller\n",
                      operation);
    EXPECT_TRUE (caller_turn);
    release.set_value ();
    const auto returned = invocation.wait_for (auto_connect_wait);
    // Always release resources before reporting a failed return bound.
    release_executor.count_down ();
    returned_executor.wait ();
    held.get ();
    invocation.get ();
    EXPECT_EQ (std::future_status::ready, returned);
    if (operation < 4)
        EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    fixture.failures->rethrow_if_failed ();
    fixture.service.stop ();
    node->stop ();
}

void check_synchronous_auto_wrappers ()
{
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-sync-peer";
    {
        SCOPED_TRACE ("route.connect");
        check_sync_owner_wait ("route.connect", auto_connect_owner_lane (route),
                               [&] { EXPECT_TRUE (route.connect (rid, endpoint)); });
    }
    {
        SCOPED_TRACE ("route.disconnect_endpoint");
        check_sync_owner_wait ("route.disconnect_endpoint", auto_connect_owner_lane (route),
                               [&] { EXPECT_TRUE (route.disconnect (rid, endpoint)); });
    }
    in_memory_location_repository_t store;
    store_location_resolvers_t cache (store);
    {
        SCOPED_TRACE ("cache.invalidate_all_routes_after_store_recovery");
        check_sync_owner_wait ("cache.invalidate_all_routes_after_store_recovery",
                               auto_connect_cache_test_access_t::lane (cache),
                               [&] { cache.invalidate_all_routes_after_store_recovery (); });
    }
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    auto &host = node.native_node ();
    auto &raw = host.transport ();
    auto &raw_lane = auto_connect_owner_lane (raw);
    mesh::service_node_descriptor_t descriptor = raw.topology ().local_descriptor ();
    descriptor.node_routing_id = rid.to_bytes ();
    descriptor.advertised_endpoint = endpoint;
    const std::pair<const char *, std::function<void ()>> raw_wrappers[] = {
      {"raw.connect_peer_intent",
       [&] { raw.connect_peer_intent (endpoint, rid, 1, "default", true); }},
      {"raw.disconnect_peer_intent", [&] { raw.disconnect_peer_intent (rid, endpoint, true); }},
      {"raw.expect_peer", [&] { raw.expect_peer (descriptor); }},
      {"raw.forget_peer", [&] { raw.forget_peer (rid.to_bytes (), endpoint); }},
      {"mesh.connect_peer", [&] { node.connect_peer (rid, endpoint, 1, "default", true); }},
      {"mesh.expect_peer", [&] { node.expect_peer (rid, endpoint, 1, "default"); }},
      {"mesh.forget_peer", [&] { node.forget_peer (rid, endpoint); }},
      {"host.expect_peer", [&] { host.expect_peer (endpoint, rid, 1, "default"); }},
      {"host.forget_peer", [&] { host.forget_peer (endpoint, rid); }}};
    for (const auto &[name, work] : raw_wrappers) {
        SCOPED_TRACE (name);
        check_sync_owner_wait (name, raw_lane, work);
    }
    {
        SCOPED_TRACE ("mesh.disconnect_peer");
        check_sync_owner_wait ("mesh.disconnect_peer", state->lane,
                               [&] { node.disconnect_peer (rid, endpoint, true); });
    }
    node.stop ();
}
} // namespace

TEST (AutoConnectSyncTest, ReconcileStopsBeforeTouchingActiveAfterTargetCompletion)
{
    for (int operation : {0, 1, 2}) {
        production_auto_fixture_t fixture;
        location_auto_connect_test_access_t::check_stop_between_targets (fixture.service,
                                                                         operation);
    }
}

TEST (AutoConnectSyncTest, StopDisconnectsConnectCompletedWhileStopIsPending)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "auto-connect-stop");
    production_auto_fixture_t fixture;
    detail::route_channel_runtime_t route ("l4-mesh");
    location_auto_connect_test_access_t::check_stop_after_pending_connect (fixture.service, runtime,
                                                                           route);
}

TEST (AutoConnectSyncTest, StopTimeoutLeavesPendingLoopToDisconnectTargets)
{
    for (int operation : {0, 1, 2}) {
        SCOPED_TRACE (operation);
        in_memory_location_repository_t store;
        location_options_t options;
        options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
        location_runtime_t runtime (store, options, "timeout-owner");
        production_auto_fixture_t fixture;
        detail::route_channel_runtime_t route ("l4-mesh");
        auto release = std::make_shared<task_completion_source_t<void>> ();
        auto disconnects = std::make_shared<std::map<std::string, unsigned>> ();
        auto finished = location_auto_connect_test_access_t::start_timeout_probe (
          fixture.service, runtime, route, release, operation, disconnects);
        fixture.service.stop ();
        try {
            fixture.failures->rethrow_if_failed ();
            ADD_FAILURE () << "stop did not report its timeout";
        }
        catch (const framework_exception_t &error) {
            EXPECT_EQ (framework_error_kind_t::deadline_exceeded, error.kind ());
        }
        EXPECT_EQ (1u, location_auto_connect_test_access_t::loop_count (fixture.service));
        release->complete (result_t<void>::success ());
        const auto result = finished.result_for (auto_connect_wait);
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
        EXPECT_TRUE (route.list_connections ().empty ());
        ASSERT_EQ (1u, disconnects->size ());
        EXPECT_EQ (1u, disconnects->at ("inproc://l4-timeout"));
    }
}

TEST (AutoConnectSyncTest, StopTimeoutStopsIndependentLocationRuntimesAndDrainsLoop)
{
    in_memory_location_repository_t repository;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
    location_runtime_t locations (repository, options, "timeout-owner");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    service_provider_t services;
    auto bus = builder.message_bus ();
    auto channel_runtime = detail::channel_runtime_t::from (bus);
    channel_runtime.bind_core_context (std::make_shared<zlink::context_t> ());
    channel_snapshot_t server_channel;
    server_channel.name = "timeout-server";
    server_channel.server.enabled = true;
    server_channel.server.bind_endpoints = {"tcp://127.0.0.1:0"};
    channel_snapshot_t publisher_channel;
    publisher_channel.name = "timeout-fanout";
    publisher_channel.publisher.enabled = true;
    publisher_channel.publisher.discovery = true;
    publisher_channel.publisher.routing_id = zlink::routing_id_t::from ("timeout-publisher");
    publisher_channel.publisher.bind_endpoints = {"inproc://timeout-fanout"};
    const std::vector<channel_snapshot_t> channels{server_channel, publisher_channel};
    auto listener_statuses = std::make_shared<listener_status_registry_t> ();
    auto client_server_runtime = std::make_shared<client_server::client_server_location_runtime_t> (
      bus, channels, locations, repository, repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, std::map<std::string, std::string>{},
      listener_statuses);
    auto fanout_runtime = std::make_shared<fanout::fanout_location_runtime_t> (
      bus, channels, locations, repository, repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, detail::handler_invocation_executor ());
    client_server_runtime->start ();
    fanout_runtime->start ();
    ASSERT_TRUE (listener_statuses->find (listener_kind_t::client_server, "timeout-server"));
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, bus, channels, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {}, {}, client_server_runtime,
      fanout_runtime, listener_statuses);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto finished = location_auto_connect_test_access_t::start_timeout_probe (service, locations,
                                                                              route, release, 0);

    service.stop ();

    EXPECT_TRUE (client_server::client_server_location_runtime_test_access_t::stop_requested (
      *client_server_runtime));
    EXPECT_TRUE (fanout::fanout_location_runtime_test_access_t::stop_requested (*fanout_runtime));
    EXPECT_FALSE (listener_statuses->find (listener_kind_t::client_server, "timeout-server"));
    EXPECT_EQ (1u, location_auto_connect_test_access_t::loop_count (service));
    EXPECT_TRUE (client_server_runtime->republish_after_store_recovery ().result ().value ());
    EXPECT_FALSE (fanout_runtime->republish_after_store_recovery ().result ().value ());

    auto replacement_publisher = std::make_shared<fanout::fanout_location_runtime_t> (
      bus, channels, locations, repository, repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, detail::handler_invocation_executor ());
    EXPECT_NO_THROW (replacement_publisher->start ());
    replacement_publisher->stop ();

    release->complete (result_t<void>::success ());
    const auto result = finished.result_for (auto_connect_wait);
    ASSERT_TRUE (result);
    EXPECT_TRUE (*result);
    EXPECT_TRUE (route.list_connections ().empty ());
}

TEST (AutoConnectSyncTest, DestructorDrainsPendingLoopAfterStopTimeout)
{
    auto executor = infrastructure_executor ();
    auto workers = test::reserve_single_infrastructure_worker (*executor);
    in_memory_location_repository_t store;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
    location_runtime_t runtime (store, options, "timeout-owner");
    std::promise<void> destructor_wait_logged;
    logging_builder_t logging;
    logging.use_callback_sink ([&] (const log_record_t &record) {
        if (record.message == "Location coordinator destructor is waiting for the loop to finish")
            destructor_wait_logged.set_value ();
    });
    auto monitoring = std::make_shared<detail::monitoring_runtime_state_t> ();
    monitoring->diagnostics_logger = logging.create_logger ("zlink.framework.runtime");
    runtime.bind_monitoring (monitoring);
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    detail::route_channel_runtime_t route ("l4-mesh");
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    auto service = std::make_unique<location_auto_connect_host_service_t> (
      failures, builder.message_bus (), std::vector<channel_snapshot_t>{}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto finished = location_auto_connect_test_access_t::start_timeout_probe (*service, runtime,
                                                                              route, release, 0);
    service->stop ();
    std::promise<void> destructor_stopping;
    auto destroying = std::async (std::launch::async, [&] {
        destructor_stopping.set_value ();
        service.reset ();
    });
    destructor_stopping.get_future ().wait ();
    EXPECT_EQ (std::future_status::ready,
               destructor_wait_logged.get_future ().wait_for (auto_connect_wait));
    EXPECT_EQ (std::future_status::timeout,
               destroying.wait_for (std::chrono::milliseconds::zero ()));
    // Host destruction runs on its owning caller. The pending loop must still
    // be able to use the sole available infrastructure worker while it waits.
    executor->submit ([release] { release->complete (result_t<void>::success ()); });
    EXPECT_EQ (std::future_status::ready, destroying.wait_for (auto_connect_wait));
    destroying.get ();
    EXPECT_TRUE (finished.result ());
    EXPECT_TRUE (route.list_connections ().empty ());
}

TEST (AutoConnectSyncDeathTest, MeshStopCompletesWithoutInfrastructureExecutor)
{
    ASSERT_EXIT (
      {
          for (int operation : {0, 1, 2, 3, 4}) {
              SCOPED_TRACE (operation);
              check_mesh_stop_without_infrastructure (operation);
          }
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectSyncDeathTest, StopCompletesWithoutCompletionDispatcher)
{
    ASSERT_EXIT (
      {
          for (bool mesh : {false, true})
              for (bool raw_owner : {false, true})
                  check_synchronous_stop (mesh, raw_owner);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectSyncDeathTest, ContendedWrappersCompleteWithoutCompletionDispatcher)
{
    ASSERT_EXIT (
      {
          check_synchronous_auto_wrappers ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (ZLinkFrameworkLocationRuntime, DrainingPublicationDoesNotHoldRecoveryMutex)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto state =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("publication-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_runtime_t runtime (*store);
    runtime.renew_owner_lease_once ();
    serializer_registry_t serializers;
    mesh_node_host_service_t service (std::make_shared<runtime_failure_collector_t> (), {state},
                                      serializers);
    mesh_node_host_service_test_access_t::configure (service, store, runtime, true);
    auto node = service.nodes ().front ();
    node->start ().result ().value ();
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto &topology = node->native_node ().transport ().topology ();
    topology.set_change_handler ([&] {
        entered.set_value ();
        released.wait ();
    });
    auto draining = std::async (std::launch::async, [&] {
        return service.publish_descriptor_state (framework_runtime_state_t::draining);
    });
    const auto pending = entered.get_future ().wait_for (std::chrono::seconds (5));
    const bool available =
      mesh_node_host_service_test_access_t::publication_mutex_available (service);
    auto recovery = std::async (std::launch::async, [&] {
        return service
          .republish_after_store_recovery (location_write_intent_t::new_claim, {},
                                           std::chrono::steady_clock::time_point::max ())
          .result ()
          .value ();
    });
    const bool recovered_while_pending =
      recovery.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release.set_value ();
    const bool recovered = recovery.get ();
    const bool drained = draining.get ();
    topology.set_change_handler ({});
    const auto descriptors = store->list_mesh_nodes ("publication-mesh").result ().value ();
    EXPECT_EQ (std::future_status::ready, pending);
    EXPECT_TRUE (available);
    EXPECT_TRUE (recovered_while_pending);
    EXPECT_TRUE (recovered);
    EXPECT_TRUE (drained);
    ASSERT_EQ (1u, descriptors.items.size ());
    EXPECT_EQ (framework_runtime_state_t::draining, descriptors.items.front ().state);
    service.stop ();
    runtime.stop ();
}

namespace zlink::framework::runtime
{
namespace
{
using namespace std::chrono_literals;
using lease_access = location_runtime_test_access_t;
using monotonic_time = std::chrono::steady_clock::time_point;

TEST (StoreOutageRetentionWait, BoundedTerminalWaitReportsMissingAndCompletedSignals)
{
    test::manual_internal_scheduler_t scheduler;
    task_completion_source_t<void> source;
    auto terminal = source.task ();
    EXPECT_FALSE (scheduler.run_until (terminal, 0ms));
    ASSERT_TRUE (
      scheduler.try_submit_internal ([&] { source.complete (result_t<void>::success ()); }));
    EXPECT_TRUE (scheduler.run_until (terminal, auto_connect_wait));
}

class outage_provider_t final : public location_store_t
{
  public:
    in_memory_location_store_t inner;
    std::vector<store_read_result_t> injected_owner_reads;
    bool hold_publication = false;
    std::atomic_uint descriptor_submissions = 0;
    task_completion_source_t<void> descriptors_submitted;
    task_completion_source_t<void> publication_release;
    bool hold_cas = false, fail_cas = false;
    std::atomic_uint preserve_writes = 0;
    task_completion_source_t<void> cas_release;
    bool fail = false;
    std::atomic_int forbidden_calls{0};
    bool forbid_io = false;
    std::string held_authority;
    std::string failed_authority;
    task_completion_source_t<void> authority_entered;
    task_completion_source_t<void> authority_release;
    unsigned authority_reads = 0;
    bool fail_next_read = false;
    bool hold_read = false;
    task_completion_source_t<void> resume;
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (key.value.starts_with (std::string_view ("authority\0", 10)))
            ++authority_reads;
        if (!failed_authority.empty () && key.value == failed_authority)
            return decltype (read (key)) (result_t<store_read_result_t>::failure (
              framework_error_kind_t::unavailable, "injected exact read error"));
        if (!held_authority.empty () && key.value == held_authority) {
            auto terminal = continue_task<store_read_result_t> (
              authority_release.task (), [this, key] { return inner.read (key); });
            authority_entered.complete (result_t<void>::success ());
            return terminal;
        }
        if (key.value.starts_with ("owner-lease") && !injected_owner_reads.empty ()) {
            auto read = std::move (injected_owner_reads.front ());
            injected_owner_reads.erase (injected_owner_reads.begin ());
            return task_t<store_read_result_t> (
              result_t<store_read_result_t>::success (std::move (read)));
        }
        if (forbid_io)
            ++forbidden_calls;
        if (fail || forbid_io || std::exchange (fail_next_read, false))
            return task_t<store_read_result_t> (result_t<store_read_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected outage read"));
        if (hold_read)
            return continue_task<store_read_result_t> (
              resume.task (),
              [this, key = std::move (key)] () mutable { return inner.read (std::move (key)); });
        return inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (hold_publication
            && std::any_of (
              request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
                  return std::visit (
                    [] (const auto &value) {
                        return value.key.value.starts_with (std::string_view ("mesh-node\0", 10));
                    },
                    mutation);
              })) {
            auto committed = inner.write (std::move (request));
            if (++descriptor_submissions == 2)
                descriptors_submitted.complete (result_t<void>::success ());
            return continue_task<store_write_result_t> (
              publication_release.task (),
              [committed = std::move (committed)] () mutable { return std::move (committed); });
        }
        const bool authority = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              return std::visit (
                [] (const auto &value) {
                    return value.key.value.starts_with (std::string_view ("authority\0", 10));
                },
                mutation);
          });
        if (authority)
            ++preserve_writes;
        if (authority && (hold_cas || fail_cas)) {
            if (fail_cas)
                return task_t<store_write_result_t> (result_t<store_write_result_t>::failure (
                  framework_error_kind_t::unavailable, "injected Preserve write failure"));
            auto committed = inner.write (std::move (request));
            auto terminal = continue_task<store_write_result_t> (
              cas_release.task (),
              [committed = std::move (committed)] () mutable { return std::move (committed); });
            return terminal;
        }
        if (forbid_io) {
            ++forbidden_calls;
            return task_t<store_write_result_t> (result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected outage write"));
        }
        return inner.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (forbid_io) {
            ++forbidden_calls;
            return task_t<store_scan_result_t> (result_t<store_scan_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected outage scan"));
        }
        return inner.scan (std::move (request));
    }
};

class outage_memory_t final : public in_memory_location_repository_t
{
  public:
    bool hold_publication = false;
    std::atomic_uint descriptor_submissions = 0;
    task_completion_source_t<void> descriptors_submitted;
    task_completion_source_t<void> publication_release;
    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        auto committed =
          in_memory_location_repository_t::update_mesh_node (std::move (descriptor), intent);
        if (!hold_publication)
            return committed;
        if (++descriptor_submissions == 2)
            descriptors_submitted.complete (result_t<void>::success ());
        return continue_task<location_write_result_t> (
          publication_release.task (),
          [committed = std::move (committed)] () mutable { return std::move (committed); });
    }
    bool hold_cas = false, fail_cas = false;
    std::atomic_uint preserve_writes = 0;
    task_completion_source_t<void> cas_release;
    bool fail = false;
    std::atomic_int forbidden_calls{0};
    bool forbid_io = false;
    std::string held_authority;
    std::string failed_authority;
    task_completion_source_t<void> authority_entered;
    task_completion_source_t<void> authority_release;
    unsigned authority_reads = 0;
    bool fail_renew = false;
    bool hold_read = false;
    bool hold_renew = false;
    std::vector<owner_lease_read_result_t> injected_owner_reads;
    task_completion_source_t<void> resume;
    task_t<authority_read_result_t> read_authority_core (authority_key_t key,
                                                         std::stop_token cancellation = {}) override
    {
        if (forbid_io) {
            ++forbidden_calls;
            return task_t<authority_read_result_t> (result_t<authority_read_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected outage authority read"));
        }
        ++authority_reads;
        if (!failed_authority.empty () && key.value == failed_authority)
            return task_t<authority_read_result_t> (result_t<authority_read_result_t>::failure (
              framework_error_kind_t::unavailable, "injected exact read error"));
        if (!held_authority.empty () && key.value == held_authority) {
            auto terminal = continue_task<authority_read_result_t> (
              authority_release.task (), [this, key, cancellation] {
                  return in_memory_location_repository_t::read_authority_core (key, cancellation);
              });
            authority_entered.complete (result_t<void>::success ());
            return terminal;
        }
        return in_memory_location_repository_t::read_authority_core (std::move (key), cancellation);
    }
    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override
    {
        if (forbid_io) {
            ++forbidden_calls;
            return task_t<authority_compare_exchange_result_t> (
              result_t<authority_compare_exchange_result_t>::failure (
                framework_error_kind_t::internal_failure, "injected outage authority CAS"));
        }
        const auto *put = std::get_if<authority_put_t> (&mutation);
        if (put && put->preservation)
            ++preserve_writes;
        if (put && put->preservation && (hold_cas || fail_cas)) {
            if (fail_cas)
                return task_t<authority_compare_exchange_result_t> (
                  result_t<authority_compare_exchange_result_t>::failure (
                    framework_error_kind_t::unavailable, "injected Preserve write failure"));
            auto committed = in_memory_location_repository_t::compare_exchange_authority_core (
              std::move (key), std::move (version), std::move (mutation), cancellation);
            auto terminal = continue_task<authority_compare_exchange_result_t> (
              cas_release.task (),
              [committed = std::move (committed)] () mutable { return std::move (committed); });
            return terminal;
        }
        return in_memory_location_repository_t::compare_exchange_authority_core (
          std::move (key), std::move (version), std::move (mutation), cancellation);
    }
    template <typename T> task_t<T> forbidden ()
    {
        ++forbidden_calls;
        return task_t<T> (result_t<T>::failure (framework_error_kind_t::internal_failure,
                                                "injected forbidden Store entry"));
    }
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh, location_page_request_t page = {}) override
    {
        if (forbid_io)
            return forbidden<location_page_t<mesh_node_descriptor_t>> ();
        return in_memory_location_repository_t::list_mesh_nodes_core (std::move (mesh),
                                                                      std::move (page));
    }
    task_t<object_reserve_result_t> reserve_core (object_reserve_request_t request,
                                                  std::stop_token cancellation = {}) override
    {
        if (forbid_io)
            return forbidden<object_reserve_result_t> ();
        return in_memory_location_repository_t::reserve_core (std::move (request), cancellation);
    }
    task_t<object_complete_creation_result_t>
    complete_creation_core (object_complete_creation_request_t request,
                            std::stop_token cancellation = {}) override
    {
        if (forbid_io)
            return forbidden<object_complete_creation_result_t> ();
        return in_memory_location_repository_t::complete_creation_core (std::move (request),
                                                                        cancellation);
    }
    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
                 std::stop_token cancellation = {},
                 std::chrono::system_clock::time_point deadline = {}) override
    {
        if (forbid_io)
            return forbidden<object_commit_result_t> ();
        return in_memory_location_repository_t::commit_core (std::move (request), cancellation,
                                                             deadline);
    }
    task_t<aggregate_prepare_result_t>
    prepare_aggregate_core (aggregate_prepare_request_t request,
                            std::stop_token cancellation = {}) override
    {
        if (forbid_io)
            return forbidden<aggregate_prepare_result_t> ();
        return in_memory_location_repository_t::prepare_aggregate_core (std::move (request),
                                                                        cancellation);
    }
    task_t<owner_lease_renew_result_t> renew_owner_lease (location_owner_token_t token,
                                                          std::chrono::milliseconds ttl) override
    {
        if (fail || fail_renew)
            return task_t<owner_lease_renew_result_t> (
              result_t<owner_lease_renew_result_t>::failure (
                framework_error_kind_t::internal_failure, "injected outage renew"));
        if (hold_renew)
            return continue_task<owner_lease_renew_result_t> (
              resume.task (), [this, token = std::move (token), ttl] () mutable {
                  return in_memory_location_repository_t::renew_owner_lease (std::move (token),
                                                                             ttl);
              });
        return in_memory_location_repository_t::renew_owner_lease (std::move (token), ttl);
    }
    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner) override
    {
        if (!injected_owner_reads.empty ()) {
            auto read = std::move (injected_owner_reads.front ());
            injected_owner_reads.erase (injected_owner_reads.begin ());
            return task_t<owner_lease_read_result_t> (
              result_t<owner_lease_read_result_t>::success (std::move (read)));
        }
        if (fail)
            return task_t<owner_lease_read_result_t> (result_t<owner_lease_read_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected outage read"));
        if (hold_read)
            return continue_task<owner_lease_read_result_t> (
              resume.task (), [this, owner = std::move (owner)] () mutable {
                  return in_memory_location_repository_t::read_owner_lease (std::move (owner));
              });
        return in_memory_location_repository_t::read_owner_lease (std::move (owner));
    }
};

template <class Result>
std::optional<authority_snapshot_t> completion_ready_snapshot (const Result &result)
{
    if constexpr (requires { result.ready; })
        return result.ready;
    else
        return std::nullopt;
}

class StoreOutageRetention : public ::testing::TestWithParam<bool>
{
  protected:
    outage_provider_t provider;
    outage_memory_t memory;
    provider_location_repository_t repository{provider};
    location_repository_t &store ()
    {
        return GetParam () ? static_cast<location_repository_t &> (repository) : memory;
    }
    // Future synthetic epoch keeps the real scheduler asleep; only advance_to dispatches it.
    std::atomic<monotonic_time> clock_now{monotonic_time{std::chrono::hours (1000000)}};

    void advance (std::chrono::milliseconds duration)
    {
        clock_now.store (clock_now.load () + duration);
    }

    void move_to (monotonic_time at) { clock_now.store (at); }
    location_options_t options;
    void registered_owner (bool shared, bool retained, int loss = 0);
    void discard_retained (bool shared, bool retained, bool rejoin = false);

    void claim (location_runtime_t &runtime)
    {
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_EQ (clock_now.load (),
                   lease_access::snapshot (runtime)->last_successful_lease->started_at);
    }
};

void StoreOutageRetention::discard_retained (bool shared, bool retained, bool rejoin)
{
    options.store_outage_retention = retained;
    location_runtime_t runtime (store (), options, "discard-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    location_lifecycle_t lifecycle (runtime);
    auto builder =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("discard-mesh");
    builder->routing_id = zlink::routing_id_t::from ("discard-node");
    builder->core_context = std::make_shared<zlink::context_t> ();
    auto node = builder->spot_state;
    node->snapshot.routing_id = builder->routing_id;
    node->snapshot.actor_types.push_back ("player");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    detail::spot_node_runtime_t owner (node);
    owner.bind_location_lifecycle (lifecycle);
    detail::mesh_node_runtime_t mesh (builder);
    mesh.start ().result ().value ();
    service_collection_t services;
    auto gateway_state = std::make_shared<detail::actor_gateway_state_t> ();
    services.add_singleton<detail::actor_gateway_runtime_t> (
      std::make_unique<detail::actor_gateway_runtime_t> (gateway_state));
    auto service_provider = services.build_provider ();
    node->root_services = service_provider;
    auto &gateway = service_provider.get_required<detail::actor_gateway_runtime_t> ();
    std::vector<std::shared_ptr<detail::spot_context_state_t>> contexts;
    // Close settles on the offload worker after its lane turn erases the context entry.
    std::vector<std::shared_ptr<std::atomic<bool>>> close_settled;
    const auto add_context = [&] (std::string id) {
        auto context = std::make_shared<detail::spot_context_state_t> ();
        auto settled = std::make_shared<std::atomic<bool>> (false);
        context->merged_close_results.push_back (
          [settled] (result_t<bool>) mutable { settled->store (true); });
        close_settled.push_back (settled);
        context->node = node;
        context->lane_owner = node;
        context->serial_executor = scheduler;
        context->spot_id = spot_id_t (id);
        context->spot_name = "room";
        context->mesh_name = "discard-mesh";
        context->node_rid = node_rid_t::from_string ("discard-node");
        context->execution_mode =
          shared ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
        contexts.push_back (context);
        node->spot_contexts_by_id.emplace (id, detail::spot_context_access_t::create (context));
        return context;
    };
    auto spot = add_context ("retained-spot");
    auto spot_finished = std::make_shared<task_completion_source_t<bool>> ();
    spot->merged_close_results.push_back ([spot_finished] (result_t<bool> result) mutable {
        spot_finished->complete (std::move (result));
    });
    auto entry = add_context ("entry");
    entry->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    std::map<std::string, std::shared_ptr<serial_execution_queue_t>> gates;
    std::map<std::string, std::weak_ptr<void>> instances;
    const auto add_actor = [&] (std::string id, auto context, bool ready) {
        auto key = "player:" + id;
        auto actor =
          ::zlink::framework::detail::actor_ref_access_t::make (context->node_rid, "player", id, 1);
        node->actor_types_by_id[id] = "player";
        node->actor_spot_ids[key] = context->spot_id;
        node->actor_generations[key] = 1;
        node->actor_instances[key] = std::make_shared<int> (1);
        instances[id] = node->actor_instances[key];
        ++context->actor_count;
        stateful::object_ref_t object{
          stateful::object_kind_t::actor, id, 1, 1, "discard-mesh", "discard-node"};
        auto creating = mesh.native_node ().create_reserved_actor_async ("player", object);
        EXPECT_TRUE (scheduler->run_until (creating));
        creating.result ().value ();
        EXPECT_TRUE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor, id));
        auto queue = context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
        if (ready)
            queue->confirm_execution_ready (clock_now.load ());
        gates.emplace (id, queue);
        gateway.bind_session_sink (actor,
                                   [] (std::string, stream_codec_t, const zlink::message_t &) {
                                       return task_t<void> (result_t<void>::success ());
                                   });
    };
    auto late_spot = add_context ("late-spot");
    auto creating_spot = add_context ("creating-spot");
    auto closing_spot = add_context ("closing-spot");
    auto relocating_spot = add_context ("relocating-spot");
    for (const auto &excluded : {closing_spot, relocating_spot}) {
        auto queue = excluded->ensure_spot_serial_executor ()->spot_queue ();
        queue->confirm_execution_ready (clock_now.load ());
        queue->invalidate_execution_ready ();
    }
    add_actor ("early-member", late_spot, true);
    add_actor ("other-member", late_spot, false);
    auto late_spot_gate = late_spot->ensure_spot_serial_executor ()->spot_queue ();
    auto spot_gate = spot->ensure_spot_serial_executor ()->spot_queue ();
    spot_gate->confirm_execution_ready (clock_now.load ());
    add_actor ("member", spot, true);
    add_actor ("late-member", spot, false);
    add_actor ("independent", entry, true);
    add_actor ("creating", entry, false);
    add_actor ("closing", entry, true);
    gates.at ("closing")->invalidate_execution_ready ();
    add_actor ("relocating", entry, true);
    gates.at ("relocating")->invalidate_execution_ready ();
    node->retiring_actor_keys.insert ("player:relocating");
    auto timer_queue = spot->ensure_spot_serial_executor ()->timer_queue ("pending-timer");
    auto timer = std::make_shared<detail::timer_state_t> ();
    timer->name = "pending-timer";
    timer->serial_queue = timer_queue;
    spot->timers.push_back (timer);
    scheduler->run_all ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    add_actor ("late", entry, true);
    late_spot_gate->confirm_execution_ready (clock_now.load ());
    provider.forbid_io = memory.forbid_io = true;
    const auto check_store_closed = [&] {
        const auto check = [] (const auto &result) {
            ASSERT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
        };
        check (store ().reserve ({}).result ());
        check (store ().read_authority (actor_authority_key ("rejoined-actor")).result ());
        check (store ().list_mesh_nodes ("discard-mesh").result ());
    };
    if (rejoin && retained) {
        move_to (*lease_access::snapshot (runtime)->admission_deadline + 1ms);
        ASSERT_LT (clock_now.load (), episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_FALSE (episode->discarded.load ());
        check_store_closed ();
    }
    int handlers = 0, completed = 0;
    std::map<std::string, int> starts;
    serial_work_options_t pending;
    pending.completion = [&] (result_t<void> result) {
        EXPECT_FALSE (timer->disposed.load ());
        EXPECT_FALSE (result);
        if (!result)
            EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
        ++completed;
    };
    for (const auto &id : {"member", "late-member", "independent", "early-member"}) {
        auto actor_pending = pending;
        actor_pending.completion = [&, id = std::string (id)] (result_t<void> result) {
            EXPECT_FALSE (instances.at (id).expired ()) << id;
            pending.completion (std::move (result));
        };
        ASSERT_TRUE (gates.at (id)->try_post (
          id,
          [&, id = std::string (id)] {
              ++starts[id];
              ++handlers;
          },
          actor_pending));
    }
    ASSERT_TRUE (spot_gate->try_post ("pending-spot", [&] { ++handlers; }, pending));
    ASSERT_TRUE (timer_queue->try_post ("pending-timer", [&] { ++handlers; }, pending));
    std::latch entered (1), release (1);
    auto held = std::async (std::launch::async, [&] {
        node->lane.run_sync ([&] {
            entered.count_down ();
            release.wait ();
        });
    });
    entered.wait ();
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
    if (rejoin)
        check_store_closed ();
    EXPECT_FALSE (spot_gate->execution_allowed ());
    EXPECT_TRUE (gates.at ("late-member")->execution_allowed ());
    EXPECT_FALSE (gates.at ("early-member")->execution_allowed ());
    EXPECT_TRUE (late_spot_gate->execution_allowed ());
    lease_access::lane (runtime).run_sync ([] {});
    scheduler->run_all ();
    EXPECT_EQ (0, handlers);
    for (const auto &id : {"member", "late-member", "independent", "early-member"})
        EXPECT_EQ (0, starts[id]) << id;
    EXPECT_EQ (0, completed);
    release.count_down ();
    held.get ();
    node->lane.run_sync ([] {});
    scheduler->run_all ();
    auto finished = spot_finished->task ();
    if (spot_gate->closed ()) {
        scheduler->run_until (finished);
        EXPECT_TRUE (finished.result ().value ());
    }
    for (const auto &excluded : {late_spot, creating_spot, closing_spot, relocating_spot}) {
        EXPECT_FALSE (excluded->closed.has_value ());
        EXPECT_FALSE (excluded->ensure_spot_serial_executor ()->spot_queue ()->closed ());
    }
    // Cleanup uses only local resources even when the Store is unavailable.
    EXPECT_TRUE (spot->closed.has_value ());
    EXPECT_TRUE (timer->disposed.load ());
    EXPECT_EQ (6, completed);
    EXPECT_EQ (0, handlers);
    EXPECT_TRUE (gates.at ("independent")->closed ());
    EXPECT_TRUE (gates.at ("member")->closed ());
    EXPECT_TRUE (gates.at ("late-member")->closed ());
    node->lane.run_sync ([&] {
        EXPECT_FALSE (node->spot_contexts_by_id.contains ("retained-spot"));
        for (const auto &id : {"independent", "member", "late-member", "early-member"})
            EXPECT_FALSE (node->actor_types_by_id.contains (id));
        for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
            EXPECT_TRUE (node->actor_types_by_id.contains (id));
    });
    for (const auto &id : {"independent", "member", "late-member", "early-member"})
        EXPECT_FALSE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor, id));
    gateway_state->sync ([&] {
        for (const auto &id : {"independent", "member", "late-member", "early-member"}) {
            EXPECT_FALSE (gateway_state->actors_by_id.contains (id));
            EXPECT_FALSE (gateway_state->bound_session_sinks.contains (id));
        }
        for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
            EXPECT_TRUE (gateway_state->actors_by_id.contains (id));
    });
    for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
        EXPECT_FALSE (gates.at (id)->closed ());
    EXPECT_EQ (0, provider.forbidden_calls.load ());
    EXPECT_EQ (0, memory.forbidden_calls.load ());
    provider.forbid_io = memory.forbid_io = false;
    auto current = lease_access::snapshot (runtime);
    auto token = *current->token;
    if (rejoin) {
        move_to (std::max (clock_now.load () + 1ms, current->last_successful_lease->started_at
                                                      + options.owner_lease_ttl
                                                      + options.owner_lease_fencing_margin));
        check_store_closed ();
        // Release the fixture Store lease to model expiry without waiting on wall time.
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          store ().release_owner_lease (token).result ().value ()));
        lease_access::heartbeat (runtime, clock_now.load ());
        const auto recovered = lease_access::snapshot (runtime);
        ASSERT_TRUE (lease_access::usable_at (runtime, clock_now.load ()));
        EXPECT_NE (token.lease_generation, recovered->token->lease_generation);
        EXPECT_FALSE (recovered->outage);
        EXPECT_TRUE (episode->discarded.load ());

        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "discard-mesh";
        descriptor.rid = zlink::routing_id_t::from ("discard-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = recovered->token->owner_id;
        descriptor.lease_generation = recovered->token->lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.security_identity = "default";
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::snapshot, true, 0}};
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (location_write_status_t::stored,
                   store ()
                     .update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, "rejoined-actor"};
        request.intent.stable_type = "player";
        request.target = {descriptor.mesh_name, node_rid_t::from_string ("discard-node"), 1,
                          *recovered->token};
        request.creating_payload = {std::byte{0}};
        request.capacity_bundle.actor_slots = 1;
        const auto reservation = store ().reserve (request).result ();
        EXPECT_TRUE (reservation);
        if (reservation)
            EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reservation.value ()));
        const auto authority =
          store ().read_authority (actor_authority_key ("rejoined-actor")).result ();
        EXPECT_TRUE (authority);
        if (authority)
            EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (authority.value ()));
        const auto nodes = store ().list_mesh_nodes ("discard-mesh").result ();
        EXPECT_TRUE (nodes);
        if (nodes)
            EXPECT_EQ (1u, nodes.value ().items.size ());
        try {
            const auto *reserved =
              reservation ? std::get_if<object_reserved_t> (&reservation.value ()) : nullptr;
            ASSERT_TRUE (reserved);
            const auto payload = encode_actor_authority_payload (actor_authority_payload_t{
              .state = actor_authority_state_t::ready,
              .stable_type = "player",
              .actor_id = "rejoined-actor",
              .current_spot_id = "entry",
              .current_spot_generation = 1,
              .current_spot_kind = actor_authority_spot_kind_t::entry,
              .owner_id = recovered->token->owner_id,
              .owner_lease_generation =
                static_cast<std::uint64_t> (recovered->token->lease_generation),
              .mesh_name = descriptor.mesh_name,
              .node_rid = entry->node_rid,
              .node_generation = 1});
            advance (1ms);
            add_actor ("rejoined-actor", entry, false);
            const auto published =
              store ().commit ({request.key, reserved->fence, payload}).result ().value ();
            EXPECT_TRUE (std::holds_alternative<object_committed_t> (published));
            gates.at ("rejoined-actor")->confirm_execution_ready (clock_now.load ());
            scheduler->run_all ();
            advance (1ms);
            lease_access::communication_failure (runtime);
            const auto second = lease_access::snapshot (runtime)->outage;
            EXPECT_NE (episode, second);
            if (second != episode) {
                EXPECT_EQ (clock_now.load (), second->observed_at);
                move_to (*recovered->admission_deadline + (retained ? 1ms : -1ms));
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                int second_calls = 0;
                ASSERT_TRUE (gates.at ("rejoined-actor")->try_post ("second-retained", [&] {
                    ++second_calls;
                }));
                scheduler->run_all ();
                EXPECT_EQ (1, second_calls);
                move_to (second->cutoff);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                EXPECT_TRUE (second->discarded.load ());
                const auto cleanup_deadline = std::chrono::steady_clock::now () + 5s;
                bool removed = false;
                while (std::chrono::steady_clock::now () < cleanup_deadline) {
                    scheduler->run_all ();
                    removed = node->lane.run_sync ([&] {
                        return !node->actor_types_by_id.contains ("rejoined-actor");
                    })
                              && !mesh.native_node ().objects ().find (
                                stateful::object_kind_t::actor, "rejoined-actor");
                    if (removed)
                        break;
                    std::this_thread::yield ();
                }
                EXPECT_TRUE (removed);
                EXPECT_FALSE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor,
                                                                   "rejoined-actor"));
                EXPECT_FALSE (gates.at ("rejoined-actor")->try_post ("second-discarded", [&] {
                    ++second_calls;
                }));
                EXPECT_EQ (1, second_calls);
            }
        }
        catch (const std::exception &error) {
            ADD_FAILURE () << "Second object setup: " << error.what ();
        }
    } else {
        ++token.lease_generation;
        lease_access::accept (runtime, token, *current->renewed_at + 30s, *current->renewed_at,
                              clock_now.load (), false);
    }
    for (const auto &id : {"independent", "member", "late-member", "early-member"})
        EXPECT_FALSE (gates.at (id)->try_post ("after-new-claim", [&] { ++handlers; }));
    EXPECT_FALSE (spot_gate->try_post ("after-new-claim", [&] { ++handlers; }));
    EXPECT_FALSE (timer_queue->try_post ("after-new-claim", [&] { ++handlers; }));
    scheduler->run_all ();
    EXPECT_EQ (0, handlers);
    EXPECT_EQ (6, completed);
    for (const auto &[_, queue] : gates)
        queue->cancel_pending ();
    for (const auto &context : contexts) {
        context->ensure_spot_serial_executor ()->close ();
        context->node.reset ();
    }
    // Teardown waits for every started Close to settle, then clears the map on its lane.
    const auto closes_settled = [&] {
        return node->lane.run_sync ([&] {
            for (std::size_t i = 0; i < contexts.size (); ++i)
                if (contexts[i]->closed.has_value () && !close_settled[i]->load ())
                    return false;
            return true;
        });
    };
    const auto close_deadline = std::chrono::steady_clock::now () + 10s;
    for (;;) {
        scheduler->run_all ();
        if (closes_settled () || std::chrono::steady_clock::now () >= close_deadline)
            break;
        std::this_thread::yield ();
    }
    EXPECT_TRUE (closes_settled ());
    node->lane.run_sync ([&] { node->spot_contexts_by_id.clear (); });
    mesh.stop ();
}

TEST_P (StoreOutageRetention, CacheIntervalRefusesNewStoreWork)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "admission-owner-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto bound = std::make_shared<serial_execution_queue_t> (scheduler);
        bound->set_execution_policy (runtime.execution_policy ());
        bound->confirm_execution_ready (clock_now.load ());
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        int handlers = 0;
        for (const auto at :
             {*lease_access::snapshot (runtime)->admission_deadline, episode->cutoff}) {
            move_to (at);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (bound->try_post ("cached-bound-message", [&] { ++handlers; }));
            scheduler.run_all ();
            EXPECT_EQ (retained ? 1 : 0, handlers);
            provider.forbid_io = memory.forbid_io = true;
            const auto check = [] (const auto &result) {
                EXPECT_FALSE (result);
                if (!result) {
                    EXPECT_NE (nullptr, result.error ());
                    if (result.error ())
                        EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
                }
            };
            check (store ().read_authority ({"uncached-route"}).result ());
            check (store ().list_mesh_nodes ("uncached-mesh").result ());
            check (store ().prepare_aggregate ({}).result ());
            check (store ().reserve ({}).result ());
            check (store ().commit ({}).result ());
            check (store ().complete_creation ({}).result ());
            check (store ()
                     .compare_exchange_authority ({"relocation"}, "old", authority_retarget_t{})
                     .result ());
            EXPECT_EQ (0, provider.forbidden_calls.load ());
            EXPECT_EQ (0, memory.forbidden_calls.load ());
            provider.forbid_io = memory.forbid_io = false;
        }
        bound->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, AdmissionGuardCountsEveryMemoryStoreEntry)
{
    memory.forbid_io = true;
    EXPECT_FALSE (memory.list_mesh_nodes ("guard").result ());
    EXPECT_FALSE (memory.reserve ({}).result ());
    EXPECT_FALSE (memory.commit ({}).result ());
    EXPECT_FALSE (memory.complete_creation ({}).result ());
    EXPECT_FALSE (memory.prepare_aggregate ({}).result ());
    EXPECT_EQ (5, memory.forbidden_calls.load ());
}

TEST_P (StoreOutageRetention, ObjectCloseKeepsShuttingDownResultInBothExecutionModes)
{
    for (bool shared : {false, true}) {
        location_runtime_t runtime (store (), options, "close-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        int handlers = 0, completed = 0;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
            ++completed;
        };
        for (const auto &queue : {spot, actor}) {
            queue->set_execution_policy (runtime.execution_policy ());
            ASSERT_TRUE (queue->try_post ("before-close", [&] { ++handlers; }, pending));
        }
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (2, completed);
    }
}

TEST_P (StoreOutageRetention, RuntimeStopCancelsRetentionDeadlineAndKeepsShuttingDown)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options, "stop-retention-owner");
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            for (const auto &gate : {spot, actor}) {
                gate->set_execution_policy (runtime.execution_policy ());
                gate->confirm_execution_ready (clock_now.load ());
            }
            scheduler.run_all ();
            unsigned discards = 0;
            runtime.on_execution_discard ([&] (auto) { ++discards; }).result ().value ();
            advance (5s);
            lease_access::communication_failure (runtime);
            const auto episode = lease_access::snapshot (runtime)->outage;
            ASSERT_TRUE (episode);
            unsigned handlers = 0;
            std::array<std::optional<result_t<void>>, 2> outcomes;
            std::vector<task_t<void>> cancellations;
            serial_work_options_t pending;
            std::size_t index = 0;
            for (const auto &gate : {spot, actor}) {
                pending.completion = [&, current = index++] (result_t<void> result) {
                    outcomes[current] = std::move (result);
                };
                ASSERT_TRUE (gate->try_post ("stop-retained", [&] { ++handlers; }, pending));
            }
            runtime.stop ();
            // A heartbeat continuation accepted after stop cannot reopen this episode.
            const auto lease = lease_access::snapshot (runtime);
            lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s,
                                  *lease->renewed_at, clock_now.load (), true, episode);
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            for (const auto &gate : {actor, spot}) {
                EXPECT_EQ (status_t::stopped, gate->execution_status ());
                cancellations.push_back (gate->cancel_pending_async ());
            }
            for (auto &cancelled : cancellations) {
                scheduler.run_until (cancelled);
                cancelled.result ().value ();
            }
            scheduler.run_all ();
            EXPECT_EQ (0u, discards);
            EXPECT_EQ (0u, handlers);
            for (const auto &outcome : outcomes) {
                ASSERT_TRUE (outcome);
                EXPECT_FALSE (*outcome);
                if (!*outcome)
                    EXPECT_EQ (framework_error_kind_t::shutting_down, outcome->error_kind ());
            }
            store ().release_owner_lease (*lease->token).result ().value ();
        }
}

TEST_P (StoreOutageRetention, RuntimeStopSuppressesAlreadyDeliveredDiscardAndKeepsNotFound)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "queued-discard-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy (runtime.execution_policy ());
    gate->confirm_execution_ready (clock_now.load ());
    scheduler.run_all ();
    serial_execution_queue_t::execution_admission_t delivered;
    unsigned requests = 0;
    runtime
      .on_execution_discard ([&] (auto policy) {
          ++requests;
          delivered = std::move (policy);
      })
      .result ()
      .value ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_EQ (1u, requests);
    ASSERT_TRUE (delivered);
    ASSERT_EQ (status_t::ownership_lost, delivered (*gate));
    runtime.stop ();
    // The owner has received the request, but has not submitted object cleanup yet.
    EXPECT_EQ (status_t::allowed, delivered (*gate));
    EXPECT_EQ (status_t::ownership_lost, gate->execution_status ());
    std::optional<result_t<void>> outcome;
    serial_work_options_t pending;
    pending.completion = [&] (result_t<void> result) { outcome = std::move (result); };
    ASSERT_TRUE (gate->try_post ("discarded-before-stop", [] {}, pending));
    auto cancelled = gate->cancel_pending_async ();
    scheduler.run_until (cancelled);
    cancelled.result ().value ();
    scheduler.run_all ();
    ASSERT_TRUE (outcome);
    EXPECT_FALSE (*outcome);
    if (!*outcome)
        EXPECT_EQ (framework_error_kind_t::not_found, outcome->error_kind ());
    EXPECT_EQ (1u, requests);
    gate->cancel_pending ();
}

TEST_P (StoreOutageRetention, RuntimeStopDrainsLatePublicationWithoutRetentionDiscard)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "stop-publication-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    unsigned requests = 0;
    runtime.on_execution_discard ([&] (auto) { ++requests; }).result ().value ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    task_completion_source_t<location_write_result_t> provider_completion;
    auto publication = runtime.descriptor_write_confirmed (
      [&] { return provider_completion.task (); }, [&] { return clock_now.load (); });
    ASSERT_FALSE (publication.await_ready ());
    auto stopped = std::async (std::launch::async, [&] { runtime.stop (); });
    // Observation completes on cancellation; physical publication is still pending.
    const auto observed = publication.result ();
    provider_completion.complete (
      result_t<location_write_result_t>::success ({location_write_status_t::stored, 1, {}}));
    stopped.get ();
    EXPECT_FALSE (observed);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_EQ (0u, requests);
}

TEST_P (StoreOutageRetention, OutagePolicyReportsOwnershipLostAtCutoff)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (store (), options, "cutoff-result-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        for (const auto &queue : {spot, actor}) {
            queue->set_execution_policy (runtime.execution_policy ());
            queue->confirm_execution_ready (clock_now.load ());
        }
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        int handlers = 0, completed = 0;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
            ++completed;
        };
        for (const auto &queue : {spot, actor})
            ASSERT_TRUE (queue->try_post ("before-cutoff", [&] { ++handlers; }, pending));
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        for (const auto &queue : {actor, spot}) {
            EXPECT_EQ (status_t::ownership_lost, queue->execution_status ());
            queue->cancel_pending ();
        }
        scheduler.run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (2, completed);
        store ().release_owner_lease (*lease_access::snapshot (runtime)->token).result ().value ();
    }
}

TEST_P (StoreOutageRetention, StoreCallGuardRejectsReadsAndWrites)
{
    provider.forbid_io = memory.forbid_io = true;
    EXPECT_FALSE (provider.read ({"guard"}).result ());
    EXPECT_FALSE (provider.write ({}).result ());
    EXPECT_FALSE (provider.scan ({}).result ());
    EXPECT_EQ (3, provider.forbidden_calls.load ());
    EXPECT_FALSE (memory.read_authority ({"guard"}).result ());
    EXPECT_FALSE (
      memory.compare_exchange_authority ({"guard"}, "v", authority_delete_t{}).result ());
    EXPECT_EQ (2, memory.forbidden_calls.load ());
}

TEST_P (StoreOutageRetention, DiscardRetainedPerActorRetention)
{
    discard_retained (false, true);
}
TEST_P (StoreOutageRetention, DiscardRetainedSpotWideRetention)
{
    discard_retained (true, true);
}
TEST_P (StoreOutageRetention, NewClaimReopensStoreAfterDiscardPerActor)
{
    discard_retained (false, true, true);
}
TEST_P (StoreOutageRetention, NewClaimReopensStoreAfterDiscardSpotWide)
{
    discard_retained (true, true, true);
}
TEST_P (StoreOutageRetention, SecondOutageDiscardsNewPerActorWithoutRetention)
{
    discard_retained (false, false, true);
}
TEST_P (StoreOutageRetention, SecondOutageDiscardsNewSpotWideWithoutRetention)
{
    discard_retained (true, false, true);
}
TEST_P (StoreOutageRetention, DiscardRetainedPerActorDefault)
{
    discard_retained (false, false);
}
TEST_P (StoreOutageRetention, DiscardRetainedSpotWideDefault)
{
    discard_retained (true, false);
}

TEST_P (StoreOutageRetention, NormalTurnsAndIneligibleObjectsDoNotReadExecutionClock)
{
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options,
                                        "clock-owner-" + std::to_string (retained)
                                          + std::to_string (shared));
            int reads = 0;
            lease_access::clock (runtime, [&] {
                ++reads;
                return clock_now.load ();
            });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            int calls = 0;
            reads = 0;
            ASSERT_TRUE (actor->try_post ("without-location-policy", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            spot->set_execution_policy (runtime.execution_policy ());
            actor->set_execution_policy (runtime.execution_policy ());
            actor->confirm_execution_ready (clock_now.load ());
            scheduler.run_all ();
            reads = 0;
            ASSERT_TRUE (actor->try_post ("without-outage", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            advance (5s);
            lease_access::communication_failure (runtime);
            reads = 0;
            EXPECT_TRUE (spot->execution_allowed ()); // No Ready evidence.
            EXPECT_EQ (0, reads);
            spot->confirm_execution_ready (clock_now.load ()); // Ready at observation.
            EXPECT_TRUE (spot->execution_allowed ());
            EXPECT_EQ (0, reads);
            EXPECT_TRUE (actor->execution_allowed ());
            EXPECT_EQ (1, reads); // Only the pre-outage Ready object reads the clock.
            EXPECT_EQ (2, calls);
            const auto snapshot = lease_access::snapshot (runtime);
            advance (1s);
            lease_access::accept (runtime, *snapshot->token, *snapshot->renewed_at + 30s,
                                  *snapshot->renewed_at, clock_now.load (), true, snapshot->outage);
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            reads = 0;
            ASSERT_TRUE (actor->try_post ("after-outage-renewal", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            EXPECT_EQ (3, calls);
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
        }
}

TEST_P (StoreOutageRetention, AdmissionDeadlineLatchesWithoutHeartbeatAndRenewalReopens)
{
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options,
                                        "deadline-owner-" + std::to_string (retained)
                                          + std::to_string (shared));
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
            auto spot = std::make_shared<serial_execution_queue_t> (
              *scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (*scheduler);
            auto timer_queue = std::make_shared<serial_execution_queue_t> (*scheduler);
            actor->attach_spot_gate (spot);
            timer_queue->attach_spot_gate (spot);
            for (auto queue : {spot, actor, timer_queue}) {
                queue->set_execution_policy (runtime.execution_policy ());
                queue->confirm_execution_ready (clock_now.load ());
            }
            runtime
              .on_execution_discard (
                [] (auto) {}, {}, {},
                [actor = std::weak_ptr (actor), timer = std::weak_ptr (timer_queue)] {
                    if (auto queue = actor.lock ())
                        queue->execution_operation_changed ();
                    if (auto queue = timer.lock ())
                        queue->execution_operation_changed ();
                })
              .result ()
              .value ();
            auto context = std::make_shared<detail::spot_context_state_t> ();
            context->serial_executor = scheduler;
            context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
            serializer_registry_t serializers;
            context->channel_runtime->serializers = &serializers;
            auto handler = std::make_shared<int> (0);
            auto timer = std::make_shared<detail::timer_state_t> ();
            timer->name = "deadline-timer";
            timer->serial_queue = timer_queue;
            timer->handler_instance = handler;
            int timer_calls = 0, actor_calls = 0;
            timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                          const timer_tick_t &) {
                ++timer_calls;
                return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
            };
            scheduler->run_all ();
            const auto first = lease_access::snapshot (runtime);
            const auto deadline = *first->admission_deadline;
            move_to (deadline - 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (actor->try_post ("before-deadline", [&] { ++actor_calls; }));
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            scheduler->run_all ();
            EXPECT_EQ (1, actor_calls);
            EXPECT_EQ (1, timer_calls);
            // Callbacks queued before the deadline must not start once the deadline reservation runs.
            ASSERT_TRUE (actor->try_post ("queued-before-deadline", [&] { ++actor_calls; }));
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            move_to (deadline + 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            scheduler->run_all ();
            EXPECT_EQ (1, actor_calls);
            EXPECT_EQ (1, timer_calls);
            EXPECT_NE (serial_execution_queue_t::execution_status_t::allowed,
                       actor->execution_status ());
            EXPECT_NE (serial_execution_queue_t::execution_status_t::allowed,
                       timer_queue->execution_status ());
            lease_access::accept (runtime, *first->token, *first->renewed_at + 30s,
                                  *first->renewed_at, clock_now.load (), true);
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            scheduler->run_all ();
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                       actor->execution_status ());
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                       timer_queue->execution_status ());
            EXPECT_EQ (2, actor_calls);
            EXPECT_EQ (2, timer_calls);
            ASSERT_TRUE (actor->try_post ("renewed", [&] { ++actor_calls; }));
            ASSERT_TRUE (timer_queue->try_post ("renewed-timer-lane", [] {}));
            scheduler->run_all ();
            EXPECT_EQ (3, actor_calls);
            EXPECT_EQ (2, timer_calls);
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            scheduler->run_all ();
            EXPECT_EQ (3, timer_calls);
            actor->cancel_pending ();
            timer_queue->cancel_pending ();
            spot->cancel_pending ();
            scheduler->run_all ();
        }
}

TEST_P (StoreOutageRetention, SecondOutageUsesNewSuccessfulLease)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "second-outage-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        std::vector<std::shared_ptr<serial_execution_queue_t>> queues;
        runtime
          .on_execution_discard ([&] (auto policy) {
              for (const auto &queue : queues)
                  if (policy (*queue)
                      == serial_execution_queue_t::execution_status_t::ownership_lost)
                      queue->cancel_pending (
                        serial_execution_queue_t::execution_status_t::ownership_lost);
          })
          .result ()
          .value ();
        const auto add = [&] {
            auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
            queue->set_execution_policy (runtime.execution_policy ());
            queue->confirm_execution_ready (clock_now.load ());
            queues.push_back (queue);
            scheduler.run_all ();
            return queue;
        };
        auto first = add ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto old = lease_access::snapshot (runtime)->outage;
        move_to (old->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        ASSERT_TRUE (old->discarded.load ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   first->execution_status ());
        store ().release_owner_lease (*lease_access::snapshot (runtime)->token).result ().value ();
        move_to (std::max (clock_now.load (),
                           lease_access::snapshot (runtime)->last_successful_lease->started_at
                             + options.owner_lease_ttl + options.owner_lease_fencing_margin));
        lease_access::heartbeat (runtime, clock_now.load ());
        const auto success = lease_access::snapshot (runtime)->last_successful_lease;
        ASSERT_NE (old->token.lease_generation, success->token.lease_generation);
        advance (1ms);
        auto second = add ();
        advance (1ms);
        lease_access::communication_failure (runtime);
        const auto current = lease_access::snapshot (runtime)->outage;
        ASSERT_NE (old, current);
        EXPECT_EQ (clock_now.load (), current->observed_at);
        EXPECT_EQ (retained ? success->started_at + options.owner_lease_ttl
                                + 2 * options.owner_lease_fencing_margin
                            : success->admission_deadline,
                   current->cutoff);
        move_to (current->cutoff - 1ms);
        int calls = 0;
        ASSERT_TRUE (second->try_post ("second-retained", [&] { ++calls; }));
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        move_to (current->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        EXPECT_TRUE (current->discarded.load ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   second->execution_status ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   first->execution_status ());
        EXPECT_FALSE (first->try_post ("old-object", [&] { ++calls; }));
        EXPECT_FALSE (second->try_post ("discarded-second", [&] { ++calls; }));
        EXPECT_EQ (1, calls);
    }
}

TEST_P (StoreOutageRetention, PausedAdmissionRejectsNewOperationAndTimerTick)
{
    for (const auto policy :
         {timer_overrun_policy_t::skip_late_ticks, timer_overrun_policy_t::catch_up_bounded,
          timer_overrun_policy_t::delay_next_tick})
        for (bool stop : {false, true})
            for (bool recovery : {false, true}) {
                location_runtime_t runtime (store (), options,
                                            "paused-admission-" + std::to_string (int (policy))
                                              + std::to_string (stop) + std::to_string (recovery));
                lease_access::clock (runtime, [&] { return clock_now.load (); });
                claim (runtime);
                auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
                auto queue = std::make_shared<serial_execution_queue_t> (*scheduler);
                queue->set_execution_policy (runtime.execution_policy ());
                queue->confirm_execution_ready (clock_now.load ());
                scheduler->run_all ();
                auto context = std::make_shared<detail::spot_context_state_t> ();
                context->serial_executor = scheduler;
                context->serial_queue = queue;
                context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
                serializer_registry_t serializers;
                context->channel_runtime->serializers = &serializers;
                auto timer = std::make_shared<detail::timer_state_t> ();
                timer->name = "paused-tick";
                timer->options.overrun_policy = policy;
                timer->options.stop_on_unhandled_exception = stop;
                timer->serial_queue = queue;
                auto timer_handler = std::make_shared<int> (0);
                timer->handler_instance = timer_handler;
                int calls = 0;
                timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                              const timer_tick_t &) {
                    ++calls;
                    return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
                };
                const auto lease = lease_access::snapshot (runtime);
                task_completion_source_t<serial_execution_queue_t::execution_operation_t> recovered;
                if (recovery)
                    queue->set_execution_operation (recovered.task ());
                else {
                    move_to (*lease->admission_deadline);
                    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                }
                auto operation = queue->try_post_cancellable_async (
                  "new-paused", [] (auto complete) { complete ([] {}); }, [] {});
                EXPECT_FALSE (operation);
                if (!operation)
                    EXPECT_EQ (framework_error_kind_t::shutting_down, operation.error_kind ());
                EXPECT_FALSE (queue->try_post ("new-paused-raw", [] {}));
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
                scheduler->run_all ();
                EXPECT_EQ (0, queue->pending_count (serial_work_lane_t::application));
                EXPECT_EQ (1, timer->pending_fire_count);
                EXPECT_FALSE (timer->pending_fire);
                EXPECT_EQ (0, timer->delivery_index);
                EXPECT_TRUE (timer->failure_events.empty ());
                EXPECT_FALSE (timer->disposed);
                if (recovery)
                    recovered.complete (
                      result_t<serial_execution_queue_t::execution_operation_t>::success (
                        {serial_execution_queue_t::execution_status_t::allowed,
                         clock_now.load ()}));
                else
                    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s,
                                          *lease->renewed_at, clock_now.load (), true);
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 0);
                scheduler->run_all ();
                EXPECT_EQ (1, calls);
                EXPECT_EQ (1, timer->delivery_index);
                timer->handler_invoker = [] (void *, void *, serializer_registry_t &,
                                             const timer_tick_t &) -> task_t<zlink::message_t> {
                    throw std::runtime_error ("actual timer handler failure");
                };
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
                scheduler->run_all ();
                ASSERT_EQ (1, timer->failure_events.size ());
                EXPECT_EQ (stop, timer->disposed.load ());
                EXPECT_EQ (stop, timer->failure_events.front ().stopped);
                queue->cancel_pending ();
                scheduler->run_all ();
            }
}

TEST_P (StoreOutageRetention, SelectedApplicationReturnsToHeadWhenPaused)
{
    for (int layout : {0, 1, 2}) {
        const bool shared = layout != 0;
        location_runtime_t runtime (store (), options,
                                    "selected-paused-" + std::to_string (layout));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::actor_delivery ());
        auto owner = queue;
        if (layout == 2) {
            queue = std::make_shared<serial_execution_queue_t> (scheduler);
            queue->attach_spot_gate (owner);
            owner->set_execution_policy (runtime.execution_policy ());
            runtime
              .on_execution_discard ([] (auto) {}, {}, {},
                                     [weak = std::weak_ptr (owner)] {
                                         if (auto queue = weak.lock ())
                                             queue->execution_operation_changed ();
                                     })
              .result ()
              .value ();
        }
        queue->set_execution_policy (runtime.execution_policy ());
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        std::vector<int> calls;
        ASSERT_TRUE (queue->try_post ("selected", [&] { calls.push_back (1); }));
        ASSERT_TRUE (queue->try_post ("following", [&] { calls.push_back (2); }));
        const auto lease = lease_access::snapshot (runtime);
        serial_execution_queue_test_access_t::select_then (*owner, [&] {
            move_to (*lease->admission_deadline);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        });
        scheduler.run_all ();
        EXPECT_TRUE (calls.empty ());
        EXPECT_EQ (2, queue->pending_count (serial_work_lane_t::application));
        lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                              clock_now.load (), true);
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        EXPECT_EQ ((std::vector<int>{1, 2}), calls);
        queue->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, YieldedResumeWaitsThroughTemporaryClosure)
{
    location_runtime_t runtime (store (), options, "yield-paused");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    int errors = 0, resumes = 0;
    auto queue = std::make_shared<serial_execution_queue_t> (
      scheduler, serial_execution_queue_options_t{},
      [&] (const std::string &, std::exception_ptr error) {
          ++errors;
          EXPECT_THROW (std::rethrow_exception (error), std::runtime_error);
      },
      serial_lane_policy_t::spot_wide ());
    queue->set_execution_policy (runtime.execution_policy ());
    runtime
      .on_execution_discard ([] (auto) {}, {}, {},
                             [weak = std::weak_ptr (queue)] {
                                 if (auto queue = weak.lock ())
                                     queue->execution_operation_changed ();
                             })
      .result ()
      .value ();
    scheduler.run_all ();
    std::shared_ptr<detail::serial_turn_t> handler;
    ASSERT_TRUE (queue->try_post_async (
      "yield-before-pause", [&] (auto) { handler = detail::capture_current_serial_turn (); }));
    scheduler.run_all ();
    ASSERT_TRUE (handler);
    ASSERT_TRUE (handler->release ());
    scheduler.run_all ();
    const auto lease = lease_access::snapshot (runtime);
    move_to (*lease->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    handler->resume_scheduler () ([&] {
        ++resumes;
        handler->finish_handler ([] {}, true);
    });
    scheduler.run_all ();
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    const auto renewed = lease_access::snapshot (runtime);
    serial_execution_queue_test_access_t::select_then (*queue, [&] {
        move_to (*renewed->admission_deadline);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    });
    scheduler.run_all ();
    auto observed = queue->observe_handler_terminal ();
    ASSERT_TRUE (observed);
    scheduler.run_all ();
    auto terminal = observed.value ()->run ();
    EXPECT_FALSE (terminal.await_ready ());
    EXPECT_EQ (0, resumes);
    EXPECT_EQ (0, errors);
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    scheduler.run_all ();
    EXPECT_EQ (1, resumes);
    scheduler.run_until (terminal);
    EXPECT_TRUE (terminal.await_ready ());
    observed.value ()->cancel ();
    ASSERT_TRUE (
      queue->try_post ("actual-handler-failure", [] { throw std::runtime_error ("handler"); }));
    scheduler.run_all ();
    EXPECT_EQ (1, errors);
    queue->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, PausedCancellationIsTerminalBeforeReopening)
{
    for (int stage : {0, 1, 2}) {
        location_runtime_t runtime (store (), options, "cancel-paused-" + std::to_string (stage));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
        queue->set_execution_policy (runtime.execution_policy ());
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        int calls = 0, cancellations = 0;
        auto submission = queue->try_post_cancellable_async (
          "cancel-paused",
          [&] (auto complete) {
              ++calls;
              complete ([] {});
          },
          [&] { ++cancellations; });
        ASSERT_TRUE (submission);
        const auto lease = lease_access::snapshot (runtime);
        const auto close = [&] {
            move_to (*lease->admission_deadline);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        };
        if (stage != 0)
            serial_execution_queue_test_access_t::select_then (*queue, [&] {
                close ();
                if (stage == 2) {
                    auto active = queue->cancel_submission (submission.value ());
                    scheduler.run_until (active);
                    EXPECT_EQ (serial_cancel_submission_outcome_t::active_cancel_requested,
                               active.result ().value ());
                }
            });
        else
            close ();
        scheduler.run_all ();
        auto cancelled = queue->cancel_submission (submission.value ());
        scheduler.run_all ();
        EXPECT_EQ (stage == 2 ? serial_cancel_submission_outcome_t::already_terminal
                              : serial_cancel_submission_outcome_t::queued_cancelled,
                   cancelled.result ().value ());
        EXPECT_EQ (1, cancellations);
        lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                              clock_now.load (), true);
        scheduler.run_all ();
        EXPECT_EQ (0, calls);
        auto again = queue->cancel_submission (submission.value ());
        scheduler.run_all ();
        EXPECT_EQ (serial_cancel_submission_outcome_t::already_terminal, again.result ().value ());
        EXPECT_EQ (1, cancellations);
        queue->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, PausedDeadlineEndsOnceWithoutExecutingAfterRenewal)
{
    location_runtime_t runtime (store (), options, "deadline-paused");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    queue->set_execution_policy (runtime.execution_policy ());
    runtime
      .on_execution_discard ([] (auto) {}, {}, {},
                             [weak = std::weak_ptr (queue)] {
                                 if (auto queue = weak.lock ())
                                     queue->execution_operation_changed ();
                             })
      .result ()
      .value ();
    scheduler.run_all ();
    int calls = 0, terminals = 0;
    task_completion_source_t<void> terminal;
    const auto lease = lease_access::snapshot (runtime);
    auto submission = queue->try_post_cancellable_async (
      "deadline-pending",
      [&] (auto complete) {
          ++calls;
          complete ([] {});
      },
      [&] {
          ++terminals;
          terminal.complete (result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                                      "operation deadline"));
      });
    ASSERT_TRUE (submission);
    move_to (*lease->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    scheduler.run_all ();
    auto deadline = std::make_shared<detail::deadline_state_t> ();
    std::optional<task_t<serial_cancel_submission_outcome_t>> cancellation;
    detail::deadline_scheduler_t::instance ().schedule (clock_now.load () + 1ms, deadline, [&] {
        cancellation.emplace (queue->cancel_submission (submission.value ()));
    });
    advance (1ms);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    scheduler.run_all ();
    ASSERT_TRUE (cancellation);
    EXPECT_EQ (serial_cancel_submission_outcome_t::queued_cancelled,
               cancellation->result ().value ());
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, terminal.task ().result ().error_kind ());
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    scheduler.run_all ();
    EXPECT_EQ (0, calls);
    EXPECT_EQ (1, terminals);
    queue->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, RetiredEpisodeFencesUntilCleanupAndNewDeadlineStillCloses)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "retired-fence");
    int clock_reads = 0;
    lease_access::clock (runtime, [&] {
        ++clock_reads;
        return clock_now.load ();
    });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto old = std::make_shared<serial_execution_queue_t> (scheduler);
    old->set_execution_policy (runtime.execution_policy ());
    old->confirm_execution_ready (clock_now.load ());
    serial_execution_queue_t::execution_admission_t cleanup;
    runtime.on_execution_discard ([&] (auto fence) { cleanup = std::move (fence); })
      .result ()
      .value ();
    scheduler.run_all ();
    advance (5s);
    lease_access::communication_failure (runtime);
    auto episode = lease_access::snapshot (runtime)->outage;
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_TRUE (cleanup);
    ASSERT_FALSE (old->closed ());
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto recovered = lease_access::snapshot (runtime);
    ASSERT_TRUE (recovered->token);
    EXPECT_FALSE (recovered->outage);
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    auto current = std::make_shared<serial_execution_queue_t> (scheduler);
    current->set_execution_policy (runtime.execution_policy ());
    current->confirm_execution_ready (clock_now.load ());
    scheduler.run_all ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, current->execution_status ());
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> verification;
    current->set_execution_operation (verification.task ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::stopped, current->execution_status ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    verification.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, clock_now.load ()}));
    scheduler.run_all ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, current->execution_status ());
    move_to (*recovered->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (current->execution_paused ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    EXPECT_FALSE (current->try_post ("expired-new-lease", [] {}));
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost, cleanup (*old));
    old->cancel_pending (serial_execution_queue_t::execution_status_t::ownership_lost);
    scheduler.run_all ();
    cleanup = {};
    episode.reset ();
    lease_access::accept (runtime, *recovered->token, *recovered->renewed_at + 30s,
                          *recovered->renewed_at, clock_now.load (), true);
    EXPECT_TRUE (lease_access::snapshot (runtime)->discarded_outages.empty ());
    clock_reads = 0;
    EXPECT_TRUE (current->execution_allowed ());
    EXPECT_EQ (0, clock_reads);
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    current->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, DelayedAdmissionCallbackDoesNotCloseRenewedLease)
{
    location_runtime_t runtime (store (), options, "delayed-deadline-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    serial_execution_queue_t queue (scheduler);
    const auto policy = runtime.execution_policy ();
    const auto first = lease_access::snapshot (runtime);
    move_to (*first->admission_deadline);
    auto callbacks = detail::deadline_scheduler_test_access_t::take_due (clock_now.load ());
    ASSERT_FALSE (callbacks.empty ());
    lease_access::accept (runtime, *first->token, *first->renewed_at + 30s, *first->renewed_at,
                          clock_now.load (), true);
    const auto renewed = lease_access::snapshot (runtime);
    ASSERT_GT (*renewed->admission_deadline, clock_now.load ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, policy (queue));
    for (auto &callback : callbacks)
        callback ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, policy (queue));
    move_to (*renewed->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::stopped, policy (queue));
}

TEST_P (StoreOutageRetention, ExecutionGateConsumesOutageLatchInBothExecutionModes)
{
    for (bool enabled : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = enabled;
            location_runtime_t runtime (store (), options,
                                        "gate-owner-" + std::to_string (enabled)
                                          + std::to_string (shared));
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            // Supply the Location latch to the gate without duplicating its deadline policy.
            const auto policy = [&runtime] (const serial_execution_queue_t &) {
                const auto episode = lease_access::snapshot (runtime)->outage;
                return !episode || !episode->discarded.load (std::memory_order_acquire)
                         ? serial_execution_queue_t::execution_status_t::allowed
                         : serial_execution_queue_t::execution_status_t::ownership_lost;
            };
            spot->set_execution_policy (policy);
            actor->set_execution_policy (policy);
            scheduler.run_all ();
            advance (5s);
            lease_access::communication_failure (runtime);
            const auto episode = lease_access::snapshot (runtime)->outage;
            int messages = 0, spot_calls = 0, completions = 0;
            move_to (episode->cutoff - 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (actor->try_post ("cached-actor", [&] { ++messages; }));
            ASSERT_TRUE (spot->try_post ("cached-spot", [&] { ++spot_calls; }));
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            serial_work_options_t queued;
            queued.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
                ++completions;
            };
            ASSERT_TRUE (actor->try_post ("queued-actor", [&] { ++messages; }, queued));
            ASSERT_TRUE (spot->try_post ("queued-spot", [&] { ++spot_calls; }, queued));
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (episode->discarded.load ());
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            EXPECT_EQ (0, completions);
            const auto lease = lease_access::snapshot (runtime);
            auto new_token = *lease->token;
            ++new_token.lease_generation;
            lease_access::accept (runtime, new_token, *lease->renewed_at + 30s, *lease->renewed_at,
                                  clock_now.load (), false);
            ASSERT_TRUE (actor->try_post ("new-claim", [&] { ++messages; }, queued));
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
            EXPECT_EQ (3, completions);
            EXPECT_EQ (0u, actor->pending_count ());
            EXPECT_EQ (0u, spot->pending_count ());
        }
}

TEST (ZLinkFrameworkLocationExecutionGate, MemberUsesItsExistingSpotGatePolicyInBothExecutionModes)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        auto other = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        other->attach_spot_gate (spot);
        auto parent = std::make_shared<const int> (1);
        auto member = std::make_shared<const int> (2);
        auto other_member = std::make_shared<const int> (3);
        const auto policy = [] (const auto &object) {
            return [weak = std::weak_ptr (object)] { return !weak.expired (); };
        };
        spot->set_execution_policy (policy (parent));
        actor->set_execution_policy (policy (member));
        other->set_execution_policy (policy (other_member));
        if (shared)
            spot->set_application_start_policy ([] (const auto &) { return false; });
        scheduler.run_all ();
        int actor_calls = 0, other_calls = 0, spot_calls = 0;
        ASSERT_TRUE (actor->try_post ("member", [&] { ++actor_calls; }));
        ASSERT_TRUE (other->try_post ("other-member", [&] { ++other_calls; }));
        ASSERT_TRUE (spot->try_post ("spot", [&] { ++spot_calls; }));
        if (shared) {
            scheduler.run_all ();
            EXPECT_EQ (0, actor_calls);
            EXPECT_EQ (0, other_calls);
            EXPECT_EQ (0, spot_calls);
        }
        member.reset ();
        spot->set_application_start_policy ([] (const auto &) { return true; });
        scheduler.run_all ();
        EXPECT_EQ (0, actor_calls);
        EXPECT_EQ (1, other_calls);
        EXPECT_EQ (1, spot_calls);
        ASSERT_TRUE (other->try_post ("parent-discarded", [&] { ++other_calls; }));
        parent.reset ();
        scheduler.run_all ();
        EXPECT_EQ (1, other_calls);
        EXPECT_TRUE (policy (other_member) ());
        actor->cancel_pending ();
        other->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0u, actor->pending_count ());
        EXPECT_EQ (0u, other->pending_count ());
        EXPECT_EQ (0u, spot->pending_count ());
    }
}

TEST (ZLinkFrameworkLocationExecutionGate, ExecutionPolicyPreservesApplicationStartPolicy)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        auto object = std::make_shared<const int> (1);
        actor->set_application_start_policy ([] (const auto &) { return false; });
        actor->set_execution_policy ([weak = std::weak_ptr (object)] { return !weak.expired (); });
        scheduler.run_all ();
        int calls = 0;
        ASSERT_TRUE (actor->try_post ("preparation-held", [&] { ++calls; }));
        scheduler.run_all ();
        EXPECT_EQ (0, calls);
        actor->set_application_start_policy ([] (const auto &) { return true; });
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        ASSERT_TRUE (actor->try_post ("execution-discarded", [&] { ++calls; }));
        object.reset ();
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, OutagePolicyAllowsLifecycleAndLateCompletionWithoutNewHandler)
{
    for (bool shared : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (
          store (), options, shared ? "shared-completion-owner" : "per-actor-completion-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        const auto policy = [&runtime] {
            const auto episode = lease_access::snapshot (runtime)->outage;
            return !episode || !episode->discarded.load (std::memory_order_acquire);
        };
        spot->set_execution_policy (policy);
        actor->set_execution_policy (policy);
        scheduler.run_all ();
        serial_execution_queue_t::async_completion_t finish;
        int started = 0, settled = 0, lifecycle = 0;
        ASSERT_TRUE (actor->try_post_async ("inflight", [&] (auto complete) {
            ++started;
            finish = std::move (complete);
        }));
        scheduler.run_all ();
        ASSERT_EQ (1, started);
        ASSERT_TRUE (finish);
        ASSERT_TRUE (actor->try_post ("unstarted", [&] { ++started; }));
        advance (5s);
        lease_access::communication_failure (runtime);
        move_to (lease_access::snapshot (runtime)->outage->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        serial_work_options_t infrastructure;
        infrastructure.lane = serial_work_lane_t::lifecycle;
        ASSERT_TRUE (spot->try_post ("cleanup", [&] { ++lifecycle; }, infrastructure));
        std::exchange (finish, {}) ([&] { ++settled; });
        scheduler.run_all ();
        EXPECT_EQ (1, started);
        EXPECT_EQ (1, settled);
        EXPECT_EQ (1, lifecycle);
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0u, actor->pending_count ());
        EXPECT_EQ (0u, spot->pending_count ());
    }
}

void StoreOutageRetention::registered_owner (bool shared, bool retained, int loss)
{
    options.store_outage_retention = retained;
    location_runtime_t runtime (store (), options, "registered-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "registered-mesh";
    descriptor.rid = zlink::routing_id_t::from ("registered-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = token.owner_id;
    descriptor.lease_generation = token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.security_identity = "default";
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::snapshot, true, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 10}};
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (location_write_status_t::stored,
               store ()
                 .update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    auto node =
      test::runtime_failure_fixture<detail::spot_node_builder_state_t> (descriptor.mesh_name);
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    const auto close_executor = [&] (const auto &executor) {
        std::vector<task_t<void>> completions;
        executor->close (&completions);
        for (auto &completion : completions)
            if (!scheduler->run_until (completion, auto_connect_wait))
                return false;
        scheduler->retire ();
        return true;
    };
    node->worker_executor = scheduler;
    mesh::service_node_descriptor_t native_descriptor;
    native_descriptor.mesh_name = descriptor.mesh_name;
    native_descriptor.node_routing_id = descriptor.rid.to_bytes ();
    native_descriptor.lifecycle_generation = 1;
    native_descriptor.descriptor_revision = 1;
    native_descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = mesh::raw_mesh_node_options_t{.descriptor = native_descriptor,
                                            .runtime_failures =
                                              std::make_shared<runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot", "player"}});
    node->native_node = native;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->serial_executor = scheduler;
    context->spot_id = spot_id_t ("registered-spot");
    context->spot_name = "room";
    context->mesh_name = descriptor.mesh_name;
    context->node_rid = node_rid_t::from_string (descriptor.rid.to_string ());
    context->execution_mode =
      shared ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
    context->object_generation = 1;
    context->authority_owner_generation = 1;
    node->spot_contexts_by_id.emplace ("registered-spot",
                                       detail::spot_context_access_t::create (context));
    for (const auto &id : {"early", "late"}) {
        const std::string key = std::string ("player:") + id;
        node->actor_types_by_id[id] = "player";
        node->actor_spot_ids[key] = context->spot_id;
        node->actor_generations[key] = 1;
    }
    detail::spot_node_runtime_t owner (node);
    location_lifecycle_t lifecycle (runtime);
    owner.bind_location_lifecycle (lifecycle);
    native->configure_location_execution (
      runtime.execution_clock (),
      [&] (stateful::object_ref_t object) {
          return owner.prepare_store_ready (std::move (object));
      },
      runtime.bound_execution_owner ());
    std::map<std::string, authority_snapshot_t> ready_snapshots;
    const auto commit = [&] (placement_object_kind_t kind, std::string id, std::string type) {
        object_reserve_request_t request;
        request.key = {kind, id};
        request.intent.stable_type = type;
        request.target = {descriptor.mesh_name, context->node_rid, 1, token};
        request.creating_payload = {std::byte{0}};
        request.capacity_bundle.actor_slots = kind == placement_object_kind_t::actor ? 1 : 0;
        request.capacity_bundle.spot_slots = kind == placement_object_kind_t::user_spot ? 1 : 0;
        if (kind == placement_object_kind_t::user_spot)
            request.capacity_bundle.spot_type = spot_type_capacity_delta_t{kind, type, 1};
        const auto reserved = store ().reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (nullptr, reservation);
        if (!reservation)
            return;
        if (kind == placement_object_kind_t::actor) {
            const stateful::object_ref_t reserved_actor{
              stateful::object_kind_t::actor,
              id,
              reservation->fence.object_generation,
              reservation->fence.authority_owner_generation,
              descriptor.mesh_name,
              descriptor.rid.to_string ()};
            native->create_reserved_actor_async (type, reserved_actor).result ().value ();
            node->actor_generations["player:" + id] = reserved_actor.object_generation;
        }
        const auto ready_payload =
          kind == placement_object_kind_t::actor
            ? encode_actor_authority_payload (actor_authority_payload_t{
                .state = actor_authority_state_t::ready,
                .stable_type = type,
                .actor_id = id,
                .current_spot_id = std::string (context->spot_id),
                .current_spot_generation = 1,
                .owner_id = token.owner_id,
                .owner_lease_generation = static_cast<std::uint64_t> (token.lease_generation),
                .mesh_name = descriptor.mesh_name,
                .node_rid = context->node_rid,
                .node_generation = 1})
            : encode_user_spot_authority_payload (user_spot_authority_payload_t{
                .state = user_spot_authority_state_t::ready,
                .stable_type = type,
                .spot_id = id,
                .owner_id = token.owner_id,
                .owner_lease_generation = static_cast<std::uint64_t> (token.lease_generation),
                .mesh_name = descriptor.mesh_name,
                .node_rid = context->node_rid,
                .node_generation = 1});
        if (kind != placement_object_kind_t::actor)
            native->get_or_create_spot (id);
        const stateful::object_ref_t object{kind == placement_object_kind_t::actor
                                              ? stateful::object_kind_t::actor
                                              : stateful::object_kind_t::user_spot,
                                            id,
                                            reservation->fence.object_generation,
                                            reservation->fence.authority_owner_generation,
                                            descriptor.mesh_name,
                                            descriptor.rid.to_string ()};
        auto confirm = native->prepare_store_ready (object).result ().value ();
        if ((loss == -22 || loss == -23) && id == "early") {
            const auto lease = std::get<owner_lease_found_t> (
              store ().read_owner_lease (token.owner_id).result ().value ());
            creation_terminal_publication_t publication{
              {context->node_rid, 1, {18, 27}}, {std::byte{7}}, lease.store_now + 1h};
            object_complete_creation_request_t completion{
              request.key, reservation->fence,
              object_creation_completed_t{ready_payload, publication}};
            const auto completed = store ().complete_creation (completion).result ().value ();
            EXPECT_TRUE (std::holds_alternative<object_creation_completed_result_t> (completed));
            const auto replay = store ().complete_creation (completion).result ().value ();
            EXPECT_TRUE (
              std::holds_alternative<object_creation_already_completed_result_t> (replay));
            const auto snapshot = std::visit (
              [] (const auto &value) { return completion_ready_snapshot (value); }, replay);
            EXPECT_TRUE (snapshot);
            if (snapshot)
                ready_snapshots[id] = *snapshot;
            if (confirm)
                confirm (snapshot).result ().value ();
            return;
        }
        const auto committed =
          store ().commit ({request.key, reservation->fence, ready_payload}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (nullptr, ready);
        if (ready) {
            ready_snapshots[id] = ready->ready;
            if (confirm)
                confirm (ready->ready).result ().value ();
        }
    };
    commit (placement_object_kind_t::user_spot, "registered-spot", "room");
    commit (placement_object_kind_t::actor, "early", "player");
    if (loss == -55 || loss == -56 || loss == -57) {
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        if (loss == -55) {
            store ().release_owner_lease (token).result ().value ();
            move_to (episode->cutoff - options.owner_lease_renew_timeout);
            provider.hold_cas = memory.hold_cas = true;
        } else {
            advance (1s);
            memory.held_authority = actor_authority_key ("early").value;
            provider.held_authority = std::string ("authority\0actor\0early", 21);
        }
        int calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (gate->try_post ("stop-held-recovery", [&] { ++calls; }, work));
        auto renewal = lease_access::heartbeat_async (runtime, clock_now.load ());
        if (loss != -55)
            (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
              .result ()
              .value ();
        renewal.result ().value ();
        // The object lane has submitted every recovery before this fence returns.
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "early"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        EXPECT_GT (lease_access::pending_store_operations (runtime), 0u);
        if (loss == -55)
            EXPECT_GT (
              GetParam () ? provider.preserve_writes.load () : memory.preserve_writes.load (), 0u);
        scheduler->run_all ();
        if (loss == -57) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            auto completed = terminal.task ();
            ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
            EXPECT_FALSE (completed.result ());
            EXPECT_EQ (framework_error_kind_t::not_found, completed.result ().error_kind ());
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        }
        task_completion_source_t<void> stopped;
        const auto stop_complete = stopped.task ();
        std::latch stop_entered (1);
        std::thread stopper ([&] {
            stop_entered.count_down ();
            runtime.stop ();
            stopped.complete (result_t<void>::success ());
        });
        stop_entered.wait ();
        // Exchange of the existing drain list proves stop reached physical drain.
        const auto drain_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (lease_access::pending_store_operations (runtime) != 0
               && !stop_complete.await_ready ()
               && std::chrono::steady_clock::now () < drain_deadline)
            std::this_thread::yield ();
        EXPECT_EQ (0u, lease_access::pending_store_operations (runtime));
        EXPECT_FALSE (stop_complete.await_ready ());
        provider.cas_release.complete (result_t<void>::success ());
        memory.cas_release.complete (result_t<void>::success ());
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        stop_complete.result ().value ();
        stopper.join ();
        ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
        EXPECT_EQ (0, calls);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -24 || loss == -25) {
        commit (placement_object_kind_t::actor, "late", "player");
        commit (placement_object_kind_t::user_spot, "next-spot", "room");
        node->snapshot.routing_id = descriptor.rid;
        service_collection_t services;
        services.add_factory<location_repository_t> (
          [&] (service_provider_t &) {
              return std::shared_ptr<location_repository_t> (&store (), [] (auto *) {});
          },
          service_lifetime_t::singleton);
        node->root_services = services.build_provider ();
        auto next = std::make_shared<detail::spot_context_state_t> ();
        next->node = node;
        next->lane_owner = node;
        next->serial_executor = scheduler;
        next->spot_id = spot_id_t ("next-spot");
        next->spot_name = "room";
        next->mesh_name = descriptor.mesh_name;
        next->node_rid = context->node_rid;
        next->object_generation = ready_snapshots.at ("next-spot").object_generation;
        next->authority_owner_generation =
          ready_snapshots.at ("next-spot").authority_owner_generation;
        next->spot_instance = std::make_shared<int> (1);
        next->lifecycle_domain = detail::spot_lifecycle_domain_t::user ();
        node->spot_contexts_by_id.emplace ("next-spot",
                                           detail::spot_context_access_t::create (next));
        node->mesh_runtime_owned_native_actor_ids.insert ("early");
        const auto actor = detail::actor_ref_access_t::make (
          context->node_rid, "player", "early",
          native->objects ().find (stateful::object_kind_t::actor, "early")->object_generation);
        actor_location_t location;
        location.mesh_name = descriptor.mesh_name;
        location.actor_id = "early";
        location.actor_ref = actor;
        EXPECT_EQ (location_write_status_t::stored, lifecycle.claim_actor (location).status);
        const auto before = ready_snapshots.at ("early").store_version;
        detail::spot_node_runtime_test_access_t::commit_membership (
          owner, node->spot_contexts_by_id.at ("next-spot"), actor)
          .result ()
          .value ();
        const auto changed = std::get<authority_snapshot_t> (
          store ().read_authority (actor_authority_key ("early")).result ().value ());
        EXPECT_NE (before, changed.store_version);
        EXPECT_EQ ("next-spot",
                   decode_direct_actor_authority_payload (changed.payload)->current_spot_id);
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto gate =
          native->objects ()
            .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "early"))
            .result ()
            .value ();
        ASSERT_TRUE (gate);
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        advance (1s);
        if (loss == -25) {
            const auto superseding = store ()
                                       .compare_exchange_authority_core (
                                         actor_authority_key ("early"), changed.store_version,
                                         authority_put_t{changed.payload})
                                       .result ()
                                       .value ();
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (superseding));
        }
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        EXPECT_EQ (loss == -24 ? serial_execution_queue_t::execution_status_t::allowed
                               : serial_execution_queue_t::execution_status_t::ownership_lost,
                   gate->execution_status ());
        gate->cancel_pending ();
        context->ensure_spot_serial_executor ()->close ();
        next->ensure_spot_serial_executor ()->close ();
        scheduler->run_all ();
        context->node.reset ();
        next->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -20 || loss == -21 || loss == -22 || loss == -23) {
        commit (placement_object_kind_t::actor, "late", "player");
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        if (loss == -20 || loss == -21) {
            auto source = *native->objects ().find (stateful::object_kind_t::actor, "early");
            auto target = source;
            stateful::public_authority_store_adapter_t adapter (store ());
            const auto snapshot = ready_snapshots.at ("early");
            const auto published =
              adapter.publish (source, target, token, snapshot.allocation.target, "adopted", 7, {},
                               snapshot.payload);
            ASSERT_EQ (stateful::authority_publish_status_t::published, published.status);
            ASSERT_TRUE (published.current);
            EXPECT_EQ (stateful::stateful_error_t::none,
                       host::public_host_runtime_test_access_t::adopt (*native, published)
                         .result ()
                         .value ());
            const auto current =
              store ().read_authority (actor_authority_key ("early")).result ().value ();
            ready_snapshots["early"] = std::get<authority_snapshot_t> (current);
        }
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        advance (1s);
        if (loss == -21 || loss == -23) {
            const auto current =
              store ().read_authority_core (actor_authority_key ("early")).result ().value ();
            const auto snapshot = std::get<authority_snapshot_t> (current);
            const auto changed = store ()
                                   .compare_exchange_authority_core (
                                     actor_authority_key ("early"), snapshot.store_version,
                                     authority_put_t{snapshot.payload})
                                   .result ()
                                   .value ();
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (changed));
        }
        provider.authority_reads = memory.authority_reads = 0;
        int early_calls = 0, late_calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (early->try_post (
          "review-fence",
          [&] {
              ++early_calls;
              terminal.complete (result_t<void>::success ());
          },
          work));
        EXPECT_TRUE (late->try_post ("review-independent", [&] { ++late_calls; }));
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        auto terminal_task = terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal_task, auto_connect_wait));
        scheduler->run_all ();
        EXPECT_EQ (3u, GetParam () ? provider.authority_reads : memory.authority_reads);
        const bool matches = loss == -20 || loss == -22;
        EXPECT_EQ (matches, terminal.task ().result ().has_value ());
        if (!matches && !terminal.task ().result ())
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.task ().result ().error_kind ());
        EXPECT_EQ (matches ? 1 : 0, early_calls);
        EXPECT_EQ (1, late_calls);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -14 || loss == -15 || loss == -16) {
        commit (placement_object_kind_t::actor, "late", "player");
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        advance (1s);
        memory.held_authority = actor_authority_key ("early").value;
        provider.held_authority = std::string ("authority\0actor\0early", 21);
        if (loss == -16) {
            memory.failed_authority = actor_authority_key ("late").value;
            provider.failed_authority = std::string ("authority\0actor\0late", 20);
        }
        provider.authority_reads = memory.authority_reads = 0;
        int calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (early->try_post (
          "held-review",
          [&] {
              ++calls;
              terminal.complete (result_t<void>::success ());
          },
          work));
        auto first = lease_access::heartbeat_async (runtime, clock_now.load ());
        (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
          .result ()
          .value ();
        // The heartbeat terminal precedes its object pass and Store submission fences.
        const bool first_completed = bool (first.result_for (auto_connect_wait));
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        const auto late_before = late->execution_status ();
        EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        advance (options.owner_lease_renew_interval);
        memory.failed_authority.clear ();
        provider.failed_authority.clear ();
        auto second = lease_access::heartbeat_async (runtime, clock_now.load ());
        const bool second_completed = bool (second.result_for (auto_connect_wait));
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        const auto reads = GetParam () ? provider.authority_reads : memory.authority_reads;
        scheduler->run_all ();
        EXPECT_EQ (0, calls);
        bool cutoff_terminal = false, cutoff_released = false;
        if (loss == -15) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            auto cutoff = terminal.task ();
            cutoff_terminal = scheduler->run_until (cutoff, auto_connect_wait);
            cutoff_released = !lease_access::snapshot (runtime)->outage;
        }
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        first.result ().value ();
        second.result ().value ();
        ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
        auto terminal_task = terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal_task, auto_connect_wait));
        scheduler->run_all ();
        EXPECT_TRUE (first_completed);
        EXPECT_TRUE (second_completed);
        EXPECT_EQ (loss == -16 ? 4u : 3u, reads);
        EXPECT_EQ (loss == -16 ? serial_execution_queue_t::execution_status_t::stopped
                               : serial_execution_queue_t::execution_status_t::allowed,
                   late_before);
        if (loss == -15) {
            EXPECT_TRUE (cutoff_terminal);
            EXPECT_TRUE (cutoff_released);
            EXPECT_FALSE (terminal.task ().result ());
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.task ().result ().error_kind ());
            EXPECT_EQ (0, calls);
        } else {
            EXPECT_TRUE (terminal.task ().result ());
            EXPECT_EQ (1, calls);
        }
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -13 || (loss <= -30 && loss >= -54)) {
        using status_t = serial_execution_queue_t::execution_status_t;
        commit (placement_object_kind_t::actor, "late", "player");
        unsigned recoveries = 0, publications = 0, second_recoveries = 0;
        auto second_owner = std::make_shared<stateful::stateful_object_runtime_t> ();
        if (loss == -47)
            runtime
              .on_execution_discard (
                [] (auto) {}, {},
                [second_owner, &second_recoveries] (
                  location_runtime_t::execution_recovery_t recover, monotonic_time observed_at) {
                    ++second_recoveries;
                    return stateful::stateful_object_runtime_t::recover_location_execution (
                      second_owner, std::move (recover), observed_at);
                })
              .result ()
              .value ();
        serializer_registry_t host_serializers;
        std::unique_ptr<mesh_node_host_service_t> mesh_host;
        if (loss == -48 || loss == -49) {
            auto mesh_state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (
              descriptor.mesh_name);
            mesh_state->routing_id = descriptor.rid;
            mesh_state->core_context = std::make_shared<zlink::context_t> ();
            mesh_host = std::make_unique<mesh_node_host_service_t> (
              std::make_shared<runtime_failure_collector_t> (), std::vector{mesh_state},
              host_serializers);
            mesh_node_host_service_test_access_t::recovery_descriptors (*mesh_host, store (),
                                                                        runtime, descriptor);
            if (loss == -49) {
                ASSERT_EQ (location_write_status_t::stored,
                           store ()
                             .remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, token)
                             .result ()
                             .value ());
                auto changed = descriptor;
                ++changed.lifecycle_generation;
                ASSERT_EQ (location_write_status_t::stored,
                           store ()
                             .update_mesh_node (changed, location_write_intent_t::new_claim)
                             .result ()
                             .value ()
                             .status);
            } else
                provider.hold_publication = memory.hold_publication = true;
        }
        task_completion_source_t<bool> publication;
        task_completion_source_t<void> submitted;
        lease_access::publisher (runtime, [&] (auto intent, auto cancellation, auto deadline) {
            ++publications;
            EXPECT_EQ (location_write_intent_t::renew, intent);
            if (mesh_host) {
                submitted.complete (result_t<void>::success ());
                return mesh_host->republish_after_store_recovery (intent, cancellation, deadline);
            }
            descriptor.owner_id = runtime.current_owner_token ()->owner_id;
            descriptor.lease_generation = runtime.current_owner_token ()->lease_generation;
            ++descriptor.descriptor_revision;
            const auto written = store ().update_mesh_node (descriptor, intent).result ().value ();
            EXPECT_EQ (location_write_status_t::stored, written.status);
            submitted.complete (result_t<void>::success ());
            return publication.task ();
        });
        auto discard_gates =
          std::make_shared<std::vector<std::weak_ptr<serial_execution_queue_t>>> ();
        runtime
          .on_execution_discard (
            [discard_gates] (auto admission) {
                for (const auto &weak : *discard_gates)
                    if (const auto queue = weak.lock ())
                        if (admission (*queue) == status_t::ownership_lost)
                            queue->cancel_pending (status_t::ownership_lost);
            },
            {},
            [&] (location_runtime_t::execution_recovery_t recover, monotonic_time observed_at) {
                ++recoveries;
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed_at);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        auto spot = executor->spot_queue ();
        *discard_gates = {gate, late, spot};
        if (loss == -47) {
            const auto object = *native->objects ().find (stateful::object_kind_t::actor, "late");
            const auto created =
              second_owner->begin_reserved_object_async (object, "player", {}).result ().value ();
            ASSERT_EQ (stateful::stateful_error_t::none, created.error);
            ASSERT_EQ (stateful::stateful_error_t::none,
                       second_owner->commit_create (created.attempt));
            native->objects ().bind_execution_gate (object, {}).result ().value ();
            second_owner->bind_execution_gate (object, late).result ().value ();
            second_owner
              ->confirm_location_ready (object, ready_snapshots.at ("late"),
                                        late->ready_confirmation (), clock_now.load ())
              .result ()
              .value ();
        }
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        if (loss == -32 || loss == -50) {
            const auto &before = ready_snapshots.at (loss == -50 ? "registered-spot" : "early");
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
              store ()
                .compare_exchange_authority_core (
                  loss == -50 ? spot_authority_key ("registered-spot")
                              : actor_authority_key ("early"),
                  before.store_version, authority_put_t{before.payload})
                .result ()
                .value ()));
        }
        store ().release_owner_lease (token).result ().value ();
        const auto claim_at = lease_access::snapshot (runtime)->last_successful_lease->started_at
                              + options.owner_lease_ttl + options.owner_lease_fencing_margin;
        move_to ((loss == -33 || loss == -35 || loss == -36 || loss == -37 || loss <= -52)
                   ? episode->cutoff - options.owner_lease_renew_timeout
                 : loss == -34 ? episode->cutoff - options.owner_lease_renew_timeout + 1ns
                               : claim_at);
        if (loss == -40 || loss == -41 || loss == -43 || loss == -45 || loss == -46 || loss == -47
            || loss <= -52)
            provider.hold_cas = memory.hold_cas = true;
        if (loss == -42)
            provider.fail_cas = memory.fail_cas = true;
        int handlers = 0, late_handlers = 0, spot_handlers = 0;
        task_completion_source_t<void> terminal, late_terminal, spot_terminal;
        const auto enqueue = [&] (auto queue, auto &signal, int &calls) {
            serial_work_options_t work;
            work.completion = [&signal] (result_t<void> result) { signal.complete (result); };
            EXPECT_TRUE (queue->try_post (
              "new-claim",
              [&signal, &calls] {
                  ++calls;
                  signal.complete (result_t<void>::success ());
              },
              work));
        };
        enqueue (gate, terminal, handlers);
        enqueue (late, late_terminal, late_handlers);
        enqueue (spot, spot_terminal, spot_handlers);
        if (loss == -38)
            runtime.stop ();
        provider.preserve_writes = memory.preserve_writes = 0;
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        // The object pass submits Store work to the runtime lane.
        lease_access::lane (runtime).run_sync ([] {});
        const auto next_token = *lease_access::snapshot (runtime)->token;
        EXPECT_NE (token.lease_generation, next_token.lease_generation);
        EXPECT_EQ (loss == -38 ? 0u : 1u, recoveries);
        EXPECT_EQ (loss == -38 ? 0u : 1u, publications);
        EXPECT_EQ (loss == -47 ? 1u : 0u, second_recoveries);
        EXPECT_EQ (loss != -38, submitted.task ().await_ready ());
        if (loss == -33)
            EXPECT_EQ (3u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        if (loss == -34 || loss == -38)
            EXPECT_EQ (0u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        if (loss == -48) {
            if (publications != 0)
                (GetParam () ? provider.descriptors_submitted.task ()
                             : memory.descriptors_submitted.task ())
                  .result ()
                  .value ();
            EXPECT_EQ (2u, GetParam () ? provider.descriptor_submissions.load ()
                                       : memory.descriptor_submissions.load ());
        }
        EXPECT_NE (status_t::allowed, gate->execution_status ());
        const auto exact = std::get<authority_snapshot_t> (
          store ().read_authority_core (actor_authority_key ("early")).result ().value ());
        const bool preserved =
          loss != -32 && loss != -34 && loss != -38 && loss != -42 && loss != -49;
        EXPECT_EQ (preserved ? next_token.lease_generation : token.lease_generation,
                   exact.owner.lease_generation);
        std::vector<task_t<serial_execution_queue_t::execution_operation_t>> operations;
        for (const auto &queue : {gate, late, spot})
            if (auto pending = queue->pending_execution_operation ())
                operations.push_back (std::move (*pending));
        const auto wait_operations = [&] {
            for (auto &operation : operations)
                if (!operation.result_for (auto_connect_wait))
                    return false;
            return true;
        };
        scheduler->run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (0, late_handlers);
        EXPECT_EQ (0, spot_handlers);
        if (loss == -35 || loss == -36 || loss == -37)
            move_to (episode->cutoff + (loss == -35 ? -1ns : loss == -36 ? 0ns : 1ns));
        if (loss == -45) {
            advance (1ms);
            auto renewed = lease_access::heartbeat_async (runtime, clock_now.load ());
            renewed.result ().value ();
            EXPECT_EQ (2u, recoveries);
            EXPECT_EQ (3u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        }
        if (loss == -48) {
            provider.publication_release.complete (result_t<void>::success ());
            memory.publication_release.complete (result_t<void>::success ());
        }
        if (loss == -46) {
            std::latch launch (3);
            auto publish = std::async (std::launch::async, [&] {
                launch.count_down ();
                launch.wait ();
                publication.complete (result_t<bool>::success (true));
            });
            auto cas = std::async (std::launch::async, [&] {
                launch.count_down ();
                launch.wait ();
                provider.cas_release.complete (result_t<void>::success ());
                memory.cas_release.complete (result_t<void>::success ());
            });
            launch.count_down ();
            launch.wait ();
            publish.get ();
            cas.get ();
        } else if (loss == -31 || loss == -41)
            publication.complete (result_t<bool>::failure (
              framework_error_kind_t::not_configured, "injected definite publication rejection"));
        else if (loss == -39)
            publication.complete (detail::result_access_t::failure<bool> (std::make_exception_ptr (
              std::runtime_error ("injected native publication failure"))));
        else
            publication.complete (result_t<bool>::success (true));
        if (loss != -34 && loss != -38 && loss != -40 && loss != -41 && loss != -43 && loss != -45
            && loss != -47 && loss > -52)
            ASSERT_TRUE (wait_operations ());
        // Flush the object lane after its operation's terminal continuation.
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        scheduler->run_all ();
        if (loss == -34 || loss == -38) {
            EXPECT_EQ (0, handlers);
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        }
        if (loss == -39 || loss == -42) {
            EXPECT_EQ (0, handlers);
            if (loss == -39) {
                EXPECT_EQ (3u, operations.size ());
                for (auto &operation : operations) {
                    const auto failed = detail::task_result_access_t::read (operation);
                    EXPECT_FALSE (failed);
                    EXPECT_EQ (nullptr, failed.error ());
                }
            }
            // A renew may reuse an in-flight pass; finish the injected failure first.
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            ASSERT_TRUE (lease_access::wait_on_lane (
              runtime,
              [&] {
                  for (const auto &queue : {gate, late, spot})
                      if (queue->pending_execution_operation ())
                          return false;
                  return true;
              },
              std::chrono::steady_clock::now () + auto_connect_wait));
            provider.fail_cas = memory.fail_cas = false;
            publication = task_completion_source_t<bool>{};
            publication.complete (result_t<bool>::success (true));
            advance (1ms);
            lease_access::heartbeat (runtime, clock_now.load ());
            lease_access::lane (runtime).run_sync ([] {});
            native->objects ()
              .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
              .result ()
              .value ();
            EXPECT_EQ (2u, recoveries);
            EXPECT_EQ (loss == -39 ? 2u : 1u, publications);
            for (const auto &queue : {gate, late, spot})
                if (auto operation = queue->pending_execution_operation ())
                    ASSERT_TRUE (operation->result_for (auto_connect_wait));
        }
        if (loss == -40 || loss == -41 || loss == -43 || loss == -45 || loss == -47
            || loss <= -52) {
            if (loss == -41)
                ASSERT_TRUE (wait_operations ());
            scheduler->run_all ();
            EXPECT_EQ (0, handlers);
            if (loss == -41)
                for (auto *signal : {&terminal, &late_terminal, &spot_terminal}) {
                    auto completed = signal->task ();
                    ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
                    const auto result = completed.result ();
                    EXPECT_FALSE (result);
                    if (!result)
                        EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
                }
            if (loss == -43) {
                advance (options.owner_lease_renew_timeout);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                native->objects ()
                  .execution_gate (
                    *native->objects ().find (stateful::object_kind_t::actor, "late"))
                  .result ()
                  .value ();
                scheduler->run_all ();
                ASSERT_TRUE (wait_operations ());
                EXPECT_EQ (status_t::stopped, gate->execution_status ());
            }
            if (loss <= -52)
                move_to (episode->cutoff + (loss == -52 ? -1ns : loss == -53 ? 0ns : 1ns));
            provider.cas_release.complete (result_t<void>::success ());
            memory.cas_release.complete (result_t<void>::success ());
            native->objects ()
              .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
              .result ()
              .value ();
            if (loss == -43) {
                move_to (episode->cutoff);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            }
        }
        if (loss != -38)
            ASSERT_TRUE (wait_operations ());
        if (loss != -38)
            for (auto *signal : {&terminal, &late_terminal, &spot_terminal}) {
                auto completed = signal->task ();
                ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
            }
        scheduler->run_all ();
        const bool allowed = loss != -31 && loss != -32 && loss != -34 && loss != -36 && loss != -37
                             && loss != -38 && loss != -41 && loss != -43 && loss != -49
                             && loss != -50 && loss != -53 && loss != -54;
        EXPECT_EQ (allowed ? 1 : 0, handlers);
        EXPECT_EQ ((allowed || loss == -32) ? 1 : 0, late_handlers);
        EXPECT_EQ ((allowed || loss == -32) ? 1 : 0, spot_handlers);
        if (loss != -38) {
            const auto check_outcome = [] (auto &signal, bool expected_allowed) {
                const auto result = signal.task ().result ();
                EXPECT_EQ (expected_allowed, result.has_value ());
                if (!expected_allowed && !result)
                    EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
            };
            check_outcome (terminal, allowed);
            check_outcome (late_terminal, allowed || loss == -32);
            check_outcome (spot_terminal, allowed || loss == -32);
        }
        if (loss == -44) {
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            advance (1s);
            lease_access::communication_failure (runtime);
            const auto second_episode = lease_access::snapshot (runtime)->outage;
            advance (1s);
            lease_access::heartbeat (runtime, clock_now.load ());
            ASSERT_TRUE (
              lease_access::recovery_terminal (runtime, second_episode, auto_connect_wait));
            ASSERT_TRUE (lease_access::wait_on_lane (
              runtime,
              [&] {
                  return !gate->pending_execution_operation ()
                         && !spot->pending_execution_operation ();
              },
              std::chrono::steady_clock::now () + auto_connect_wait));
            scheduler->run_all ();
            EXPECT_EQ (status_t::allowed, gate->execution_status ());
            EXPECT_EQ (status_t::allowed, spot->execution_status ());
        }
        if (mesh_host) {
            provider.hold_publication = memory.hold_publication = false;
            provider.publication_release.complete (result_t<void>::success ());
            memory.publication_release.complete (result_t<void>::success ());
            runtime.stop ();
        }
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss <= -10) {
        commit (placement_object_kind_t::actor, "late", "player");
        // Install recovery through the same owner registry used by the host.
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover,
                      monotonic_time observed_at) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed_at);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto spot = executor->spot_queue ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        scheduler->run_all ();
        const auto key = actor_authority_key ("early");
        if (loss == -11) {
            const auto removed =
              store ()
                .compare_exchange_authority (key, ready_snapshots.at ("early").store_version,
                                             authority_delete_t{})
                .result ()
                .value ();
            ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (removed));
        }
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        advance (1s);
        node->actor_types_by_id["new"] = "player";
        node->actor_spot_ids["player:new"] = context->spot_id;
        node->actor_generations["player:new"] = 1;
        commit (placement_object_kind_t::actor, "new", "player");
        auto fresh = executor->actor_executor ("player:new")->queue ();
        const auto held = loss == -12 ? spot_authority_key ("registered-spot") : key;
        memory.held_authority = held.value;
        provider.held_authority = std::string ("authority\0", 10)
                                  + (loss == -12 ? std::string ("spot\0registered-spot", 20)
                                                 : std::string ("actor\0early", 11));
        provider.authority_reads = memory.authority_reads = 0;
        int early_calls = 0, late_calls = 0, spot_calls = 0, fresh_calls = 0;
        task_completion_source_t<void> early_terminal, late_terminal, spot_terminal, fresh_terminal;
        serial_work_options_t early_work;
        early_work.completion = [&] (result_t<void> result) {
            early_terminal.complete (std::move (result));
        };
        EXPECT_TRUE (early->try_post (
          "held-exact",
          [&] {
              ++early_calls;
              early_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { late_terminal.complete (result); };
        EXPECT_TRUE (late->try_post (
          "matching-exact",
          [&] {
              ++late_calls;
              late_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { spot_terminal.complete (result); };
        EXPECT_TRUE (spot->try_post (
          "spot-exact",
          [&] {
              ++spot_calls;
              spot_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { fresh_terminal.complete (result); };
        EXPECT_TRUE (fresh->try_post (
          "created-after-outage",
          [&] {
              ++fresh_calls;
              fresh_terminal.complete (result_t<void>::success ());
          },
          early_work));
        auto renewing = std::async (std::launch::async, [&] {
            lease_access::heartbeat (runtime, clock_now.load ());
            // Also release the observation boundary in the unfixed implementation.
            provider.authority_entered.complete (result_t<void>::success ());
            memory.authority_entered.complete (result_t<void>::success ());
        });
        (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
          .result ()
          .value ();
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        scheduler->run_all ();
        EXPECT_EQ (loss == -12 ? 0 : 1, fresh_calls);
        EXPECT_EQ (0, early_calls);
        const auto late_ready = late->execution_ready_at ();
        EXPECT_TRUE (late_ready);
        if (late_ready)
            EXPECT_GE (*late_ready, episode->observed_at);
        if (loss == -12) {
            EXPECT_EQ (0, late_calls);
            EXPECT_EQ (0, spot_calls);
        } else {
            EXPECT_EQ (1, late_calls);
            EXPECT_EQ (1, spot_calls);
        }
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        renewing.get ();
        auto terminal = early_terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal, auto_connect_wait));
        for (auto *signal : {&late_terminal, &spot_terminal, &fresh_terminal}) {
            auto completed = signal->task ();
            ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
        }
        scheduler->run_all ();
        if (loss == -11) {
            EXPECT_FALSE (terminal.result ());
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.result ().error_kind ());
        } else
            EXPECT_TRUE (terminal.result ());
        EXPECT_EQ (loss == -11 ? 0 : 1, early_calls);
        EXPECT_EQ (1, late_calls);
        EXPECT_EQ (1, spot_calls);
        EXPECT_EQ (1, fresh_calls);
        EXPECT_EQ (3u, GetParam () ? provider.authority_reads : memory.authority_reads);
        EXPECT_EQ (loss == -11 ? serial_execution_queue_t::execution_status_t::ownership_lost
                               : serial_execution_queue_t::execution_status_t::allowed,
                   early->execution_status ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                   late->execution_status ());
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -3) {
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        context->actor_count = 1;
        node->actor_instances["player:early"] = std::make_shared<int> (1);
        runtime
          .on_execution_discard ([weak = std::weak_ptr (node)] (auto policy) {
              if (auto state = weak.lock ())
                  detail::spot_node_runtime_t (state).request_location_execution_discard (
                    std::move (policy));
          })
          .result ()
          .value ();
        scheduler->run_all ();
        ASSERT_TRUE (gate->execution_ready_at ());
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        std::latch entered (1), release (1);
        auto blocked = std::async (std::launch::async, [&] {
            node->lane.run_sync ([&] {
                entered.count_down ();
                release.wait ();
            });
        });
        entered.wait ();
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        runtime.stop ();
        release.count_down ();
        blocked.get ();
        // Fence the owner's queued discard decision before observing its worker submissions.
        node->lane.run_sync ([] {});
        EXPECT_EQ (0u, scheduler->pending_work ());
        EXPECT_FALSE (context->closed);
        EXPECT_EQ (1u, context->actor_count);
        EXPECT_TRUE (node->actor_instances.contains ("player:early"));
        gate->cancel_pending ();
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        node->native_node.reset ();
        return;
    }
    if (loss == -1) {
        auto executor = context->ensure_spot_serial_executor ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        move_to (*lease_access::snapshot (runtime)->admission_deadline);
        protocol::spot_route_fence_t route{.spot_id = "registered-spot",
                                           .object_generation = 1,
                                           .target_node_routing_id = descriptor.rid.to_bytes (),
                                           .target_node_generation = 1,
                                           .authority_owner_generation = 1,
                                           .owner_lease_generation =
                                             static_cast<std::uint64_t> (token.lease_generation)};
        stateful::accepted_record_authority_query_t actor_query;
        actor_query.target = *native->objects ().find (stateful::object_kind_t::actor, "early");
        actor_query.requested_authority_owner_generation =
          actor_query.target.authority_owner_generation;
        actor_query.target_node_routing_id = descriptor.rid.to_bytes ();
        actor_query.target_node_generation = 1;
        actor_query.source_node_routing_id = descriptor.rid.to_bytes ();
        actor_query.source_node_generation = 1;
        actor_query.source_original =
          mesh::request_source_t{std::make_shared<const mesh_node_descriptor_t> (descriptor)};
        provider.forbid_io = memory.forbid_io = true;
        const auto actor_authority =
          native->accepted_record_authority (actor_query, runtime.current_owner_observation ())
            .result ()
            .value ();
        EXPECT_EQ (retained, actor_authority.has_value ());
        auto prepared = owner.prepare_spot_dispatch ("registered-spot", route).result ();
        EXPECT_EQ (retained, bool (prepared));
        if (prepared)
            prepared.value ()->terminal ();
        EXPECT_EQ (0, provider.forbidden_calls.load ());
        EXPECT_EQ (0, memory.forbidden_calls.load ());
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_FALSE (owner.prepare_spot_dispatch ("registered-spot", route).result ());
        EXPECT_FALSE (
          native->accepted_record_authority (actor_query, runtime.current_owner_observation ())
            .result ()
            .value ());
        provider.forbid_io = memory.forbid_io = false;
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss != 0) {
        node->snapshot.routing_id = descriptor.rid;
        context->actor_count = 2;
        commit (placement_object_kind_t::actor, "late", "player");
        node->actor_instances["player:early"] = std::make_shared<int> (1);
        node->actor_instances["player:late"] = std::make_shared<int> (2);
        const std::weak_ptr<void> early_instance = node->actor_instances.at ("player:early");
        const std::weak_ptr<void> late_instance = node->actor_instances.at ("player:late");
        runtime
          .on_execution_discard (
            [weak = std::weak_ptr (node)] (auto policy) {
                if (auto state = weak.lock ())
                    detail::spot_node_runtime_t (state).request_location_execution_discard (
                      std::move (policy));
            },
            [weak = std::weak_ptr (node),
             owner_id = runtime.owner_id ()] (authority_key_t key, authority_snapshot_t snapshot) {
                if (auto state = weak.lock ())
                    detail::spot_node_runtime_t (state).observe_location_authority (
                      std::move (key), std::move (snapshot), owner_id);
            })
          .result ()
          .value ();
        auto finished = std::make_shared<task_completion_source_t<bool>> ();
        context->merged_close_results.push_back (
          [finished] (result_t<bool> result) { finished->complete (std::move (result)); });
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        auto spot_gate = executor->spot_queue ();
        const auto key =
          loss % 2 == 0 ? spot_authority_key ("registered-spot") : actor_authority_key ("early");
        const auto before = store ().read_authority (key).result ().value ();
        const auto snapshot = std::get<authority_snapshot_t> (before);
        const authority_mutation_t mutation =
          loss > 2 ? authority_mutation_t{authority_retarget_t{snapshot.payload,
                                                               snapshot.allocation.target}}
                   : authority_mutation_t{authority_reincarnate_t{snapshot.payload}};
        const auto changed = store ()
                               .compare_exchange_authority (key, snapshot.store_version, mutation)
                               .result ()
                               .value ();
        ASSERT_TRUE (std::holds_alternative<authority_stored_t> (changed));
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        int handlers = 0;
        std::atomic_int cancelled{0};
        auto cancellations = std::make_shared<task_completion_source_t<void>> ();
        auto cancellations_finished = cancellations->task ();
        const auto expected_cancellations = loss % 2 == 0 ? 2 : 1;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            if (!result && result.error ())
                EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
            if (cancelled.fetch_add (1) + 1 == expected_cancellations)
                cancellations->complete (result_t<void>::success ());
        };
        ASSERT_TRUE (early->try_post ("lost-actor", [&] { ++handlers; }, pending));
        if (loss % 2 == 0)
            ASSERT_TRUE (spot_gate->try_post ("lost-spot", [&] { ++handlers; }, pending));
        // This exact read confirms a changed incarnation before T.
        store ().read_authority (key).result ().value ();
        lease_access::lane (runtime).run_sync ([] {});
        node->lane.run_sync ([] {});
        native->objects ().find_async (stateful::object_kind_t::actor, "early").result ().value ();
        node->lane.run_sync ([] {});
        provider.forbid_io = memory.forbid_io = true;
        scheduler->run_all ();
        ASSERT_TRUE (scheduler->run_until (cancellations_finished, auto_connect_wait));
        if (loss % 2 == 0 && spot_gate->closed ()) {
            auto closed = finished->task ();
            ASSERT_TRUE (scheduler->run_until (closed, auto_connect_wait));
            EXPECT_TRUE (closed.result ().value ());
        }
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (expected_cancellations, cancelled.load ());
        EXPECT_TRUE (early->closed ());
        if (loss % 2 == 0) {
            EXPECT_TRUE (late->closed ());
            EXPECT_TRUE (spot_gate->closed ());
        } else {
            ASSERT_TRUE (late->try_post ("other-preserved", [&] { ++handlers; }));
            scheduler->run_all ();
            EXPECT_EQ (1, handlers);
            EXPECT_FALSE (late_instance.expired ());
            EXPECT_FALSE (episode->discarded.load ());
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            lease_access::lane (runtime).run_sync ([] {});
            node->lane.run_sync ([] {});
            scheduler->run_all ();
            EXPECT_TRUE (late->closed ());
            auto closed = finished->task ();
            ASSERT_TRUE (scheduler->run_until (closed, auto_connect_wait));
            EXPECT_TRUE (closed.result ().value ());
        }
        EXPECT_TRUE (early_instance.expired ());
        EXPECT_TRUE (late_instance.expired ());
        EXPECT_EQ (0, provider.forbidden_calls.load ());
        EXPECT_EQ (0, memory.forbidden_calls.load ());
        provider.forbid_io = memory.forbid_io = false;
        const auto token_before_claim = *lease_access::snapshot (runtime)->token;
        store ().release_owner_lease (token_before_claim).result ().value ();
        // Object discard does not authorize an owner claim before normal expiry.
        move_to (std::max (clock_now.load (),
                           lease_access::snapshot (runtime)->last_successful_lease->started_at
                             + options.owner_lease_ttl + options.owner_lease_fencing_margin));
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_NE (token_before_claim.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        lease_access::heartbeat (runtime, clock_now.load ());
        EXPECT_FALSE (early->try_post ("cannot-resume", [&] { ++handlers; }));
        if (loss % 2 == 0)
            EXPECT_FALSE (spot_gate->try_post ("cannot-resume-spot", [&] { ++handlers; }));
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    std::vector<std::shared_ptr<detail::spot_context_state_t>> excluded_spots;
    auto closing = std::make_shared<task_completion_source_t<host::spot_close_commit_t>> ();
    auto closing_finished = std::make_shared<task_completion_source_t<bool>> ();
    for (const auto &id : {"creating-spot", "closing-spot", "source-spot", "target-spot"}) {
        auto excluded = std::make_shared<detail::spot_context_state_t> ();
        excluded->node = node;
        excluded->lane_owner = node;
        excluded->serial_executor = scheduler;
        excluded->spot_id = spot_id_t (id);
        excluded->spot_name = "room";
        excluded->mesh_name = descriptor.mesh_name;
        excluded->node_rid = context->node_rid;
        excluded->object_generation = 1;
        excluded->authority_owner_generation = 1;
        excluded->execution_mode = context->execution_mode;
        node->spot_contexts_by_id.emplace (id, detail::spot_context_access_t::create (excluded));
        if (std::string_view (id) != "creating-spot")
            commit (placement_object_kind_t::user_spot, id, "room");
        if (std::string_view (id) == "closing-spot") {
            auto late_confirmation =
              native
                ->prepare_store_ready ({stateful::object_kind_t::user_spot, id, 1, 1,
                                        descriptor.mesh_name, descriptor.rid.to_string ()})
                .result ()
                .value ();
            excluded->request_close ([closing] { return closing->task (); },
                                     [finished = closing_finished] (result_t<bool> result) mutable {
                                         finished->complete (std::move (result));
                                     });
            scheduler->run_all ();
            EXPECT_NE (0u, excluded->close_reservation);
            ASSERT_TRUE (late_confirmation);
            late_confirmation (std::nullopt).result ().value ();
        }
        if (std::string_view (id) == "source-spot" || std::string_view (id) == "target-spot") {
            const auto object = native->objects ().find (stateful::object_kind_t::user_spot, id);
            ASSERT_TRUE (object);
            const auto seal =
              native->objects ().try_seal_relocation_aggregate ({*object}).result ().value ();
            ASSERT_EQ (stateful::stateful_error_t::none, seal.error);
        }
        excluded_spots.push_back (excluded);
    }
    const auto creating = native->objects ().begin_create (
      {stateful::object_kind_t::actor, "creating-actor", "player", descriptor.mesh_name});
    ASSERT_EQ (stateful::stateful_error_t::none, creating.error);
    node->actor_types_by_id["creating-actor"] = "player";
    node->actor_spot_ids["player:creating-actor"] = context->spot_id;
    node->actor_generations["player:creating-actor"] = creating.object.object_generation;
    auto creating_gate =
      context->ensure_spot_serial_executor ()->actor_executor ("player:creating-actor")->queue ();
    EXPECT_FALSE (native->objects ().find (stateful::object_kind_t::actor, "creating-actor"));
    auto executor = context->ensure_spot_serial_executor ();
    auto early = executor->actor_executor ("player:early")->queue ();
    auto late = executor->actor_executor ("player:late")->queue ();
    auto timer = executor->timer_queue ("registered-timer");
    advance (5s);
    // A source that performed Store I/O would fail here.
    provider.fail = memory.fail = true;

    lease_access::communication_failure (runtime);

    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    auto new_timer = executor->timer_queue ("after-snapshot-timer");
    provider.fail = memory.fail = false;
    // This later CAS has exactly observed_at: eligibility requires a strict earlier time.
    commit (placement_object_kind_t::actor, "late", "player");
    move_to (episode->cutoff - std::chrono::steady_clock::duration{1});
    int early_calls = 0, late_calls = 0, timer_calls = 0, new_timer_calls = 0;
    ASSERT_TRUE (early->try_post ("early", [&] { ++early_calls; }));
    ASSERT_TRUE (timer->try_post ("timer", [&] { ++timer_calls; }));
    ASSERT_TRUE (new_timer->try_post ("new-timer", [&] { ++new_timer_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, new_timer_calls);
    EXPECT_EQ (1, early_calls);
    EXPECT_EQ (1, timer_calls);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_TRUE (episode->discarded.load ());
    EXPECT_TRUE (creating_gate->execution_allowed ());
    for (const auto &excluded : excluded_spots) {
        const auto gate = excluded->ensure_spot_serial_executor ()->spot_queue ();
        EXPECT_TRUE (gate->execution_allowed ());
    }
    ASSERT_TRUE (early->try_post ("at-cutoff", [&] { ++early_calls; }));
    ASSERT_TRUE (late->try_post ("late-ready", [&] { ++late_calls; }));
    ASSERT_TRUE (timer->try_post ("at-cutoff-timer", [&] { ++timer_calls; }));
    ASSERT_TRUE (new_timer->try_post ("at-cutoff-new-timer", [&] { ++new_timer_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, early_calls);
    EXPECT_EQ (0, late_calls);
    EXPECT_EQ (1, timer_calls);
    EXPECT_EQ (1, new_timer_calls);
    lease_access::heartbeat (runtime, clock_now.load ());
    auto next_token = *lease_access::snapshot (runtime)->token;
    ++next_token.lease_generation;
    lease_access::accept (runtime, next_token, std::chrono::system_clock::time_point{} + 30s,
                          std::chrono::system_clock::time_point{}, clock_now.load (), false);
    const auto early_ref = native->objects ().find (stateful::object_kind_t::actor, "early");
    ASSERT_TRUE (early_ref);
    auto replay_confirmation = native->prepare_store_ready (*early_ref).result ().value ();
    ASSERT_TRUE (replay_confirmation);
    replay_confirmation (std::nullopt).result ().value ();
    early->invalidate_execution_ready ();
    early->confirm_execution_ready (clock_now.load ());
    ASSERT_TRUE (early->try_post ("recovered-new-claim", [&] { ++early_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, early_calls);
    early->cancel_pending ();
    late->cancel_pending ();
    executor->spot_queue ()->cancel_pending ();
    timer->cancel_pending ();
    new_timer->cancel_pending ();
    scheduler->run_all ();
    closing->complete (result_t<host::spot_close_commit_t>::success ({}));
    auto close_terminal = closing_finished->task ();
    ASSERT_TRUE (scheduler->run_until (close_terminal, auto_connect_wait));
    EXPECT_FALSE (close_terminal.result ().value ());
    scheduler->run_all ();
    for (const auto &excluded : excluded_spots) {
        excluded->ensure_spot_serial_executor ()->spot_queue ()->cancel_pending ();
        excluded->node.reset ();
    }
    creating_gate->cancel_pending ();
    scheduler->run_all ();
    node->spot_contexts_by_id.clear ();
    node->native_node.reset ();
    context->node.reset ();
}

TEST_P (StoreOutageRetention, NewClaimRecoveryPreservesAfterPublishAndCasPerActor)
{
    registered_owner (false, true, -13);
}
TEST_P (StoreOutageRetention, NewClaimRecoveryPreservesAfterPublishAndCasSpotWide)
{
    registered_owner (true, true, -13);
}

TEST_P (StoreOutageRetention, NewClaimPublicationRejectsAllPerActor)
{
    registered_owner (false, true, -31);
}

TEST_P (StoreOutageRetention, NewClaimPublicationRejectsAllSpotWide)
{
    registered_owner (true, true, -31);
}

TEST_P (StoreOutageRetention, NewClaimCasRejectsOnlyItsObjectPerActor)
{
    registered_owner (false, true, -32);
}

TEST_P (StoreOutageRetention, NewClaimCasRejectsOnlyItsObjectSpotWide)
{
    registered_owner (true, true, -32);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineEqualsCutoffSubmitsPerActor)
{
    registered_owner (false, true, -33);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineEqualsCutoffSubmitsSpotWide)
{
    registered_owner (true, true, -33);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineExceedsCutoffDoesNotSubmitPerActor)
{
    registered_owner (false, true, -34);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineExceedsCutoffDoesNotSubmitSpotWide)
{
    registered_owner (true, true, -34);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationBeforeCutoffPerActor)
{
    registered_owner (false, true, -35);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationBeforeCutoffSpotWide)
{
    registered_owner (true, true, -35);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAtCutoffPerActor)
{
    registered_owner (false, true, -36);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAtCutoffSpotWide)
{
    registered_owner (true, true, -36);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAfterCutoffPerActor)
{
    registered_owner (false, true, -37);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAfterCutoffSpotWide)
{
    registered_owner (true, true, -37);
}

TEST_P (StoreOutageRetention, NewClaimStopDoesNotSubmitCasPerActor)
{
    registered_owner (false, true, -38);
}

TEST_P (StoreOutageRetention, NewClaimStopDoesNotSubmitCasSpotWide)
{
    registered_owner (true, true, -38);
}

TEST_P (StoreOutageRetention, NewClaimUnconfirmedPublicationDoesNotResumePerActor)
{
    registered_owner (false, true, -39);
}

TEST_P (StoreOutageRetention, NewClaimUnconfirmedPublicationDoesNotResumeSpotWide)
{
    registered_owner (true, true, -39);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationWaitsForPublishPerActor)
{
    registered_owner (false, true, -40);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationWaitsForPublishSpotWide)
{
    registered_owner (true, true, -40);
}

TEST_P (StoreOutageRetention, NewClaimPublishRejectsWhileCasPendingPerActor)
{
    registered_owner (false, true, -41);
}

TEST_P (StoreOutageRetention, NewClaimPublishRejectsWhileCasPendingSpotWide)
{
    registered_owner (true, true, -41);
}

TEST_P (StoreOutageRetention, NewClaimCasFailureRetriesOnRenewPerActor)
{
    registered_owner (false, true, -42);
}

TEST_P (StoreOutageRetention, NewClaimCasFailureRetriesOnRenewSpotWide)
{
    registered_owner (true, true, -42);
}

TEST_P (StoreOutageRetention, NewClaimCasTimeoutRemainsFailedPerActor)
{
    registered_owner (false, true, -43);
}

TEST_P (StoreOutageRetention, NewClaimCasTimeoutRemainsFailedSpotWide)
{
    registered_owner (true, true, -43);
}

TEST_P (StoreOutageRetention, NewClaimSuccessfulCasUpdatesPreservedFencePerActor)
{
    registered_owner (false, true, -44);
}

TEST_P (StoreOutageRetention, NewClaimSuccessfulCasUpdatesPreservedFenceSpotWide)
{
    registered_owner (true, true, -44);
}

TEST_P (StoreOutageRetention, NewClaimRenewDoesNotWaitForCasPerActor)
{
    registered_owner (false, true, -45);
}

TEST_P (StoreOutageRetention, NewClaimRenewDoesNotWaitForCasSpotWide)
{
    registered_owner (true, true, -45);
}

TEST_P (StoreOutageRetention, NewClaimConcurrentConfirmationsPerActor)
{
    registered_owner (false, true, -46);
}

TEST_P (StoreOutageRetention, NewClaimConcurrentConfirmationsSpotWide)
{
    registered_owner (true, true, -46);
}

TEST_P (StoreOutageRetention, NewClaimCombinesIndependentOwnersPerActor)
{
    registered_owner (false, true, -47);
}

TEST_P (StoreOutageRetention, NewClaimCombinesIndependentOwnersSpotWide)
{
    registered_owner (true, true, -47);
}

TEST_P (StoreOutageRetention, NewClaimHostSubmitsDescriptorsBeforeCasPerActor)
{
    registered_owner (false, true, -48);
}

TEST_P (StoreOutageRetention, NewClaimHostSubmitsDescriptorsBeforeCasSpotWide)
{
    registered_owner (true, true, -48);
}

TEST_P (StoreOutageRetention, NewClaimHostRejectionIsTerminalPerActor)
{
    registered_owner (false, true, -49);
}

TEST_P (StoreOutageRetention, NewClaimHostRejectionIsTerminalSpotWide)
{
    registered_owner (true, true, -49);
}

TEST_P (StoreOutageRetention, NewClaimSpotCasRejectionDiscardsLocalMembersPerActor)
{
    registered_owner (false, true, -50);
}

TEST_P (StoreOutageRetention, NewClaimSpotCasRejectionDiscardsLocalMembersSpotWide)
{
    registered_owner (true, true, -50);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationBeforeCutoffPerActor)
{
    registered_owner (false, true, -52);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationBeforeCutoffSpotWide)
{
    registered_owner (true, true, -52);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAtCutoffPerActor)
{
    registered_owner (false, true, -53);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAtCutoffSpotWide)
{
    registered_owner (true, true, -53);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAfterCutoffPerActor)
{
    registered_owner (false, true, -54);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAfterCutoffSpotWide)
{
    registered_owner (true, true, -54);
}

TEST_P (StoreOutageRetention, SameTokenExactMatchPerActor)
{
    registered_owner (false, true, -10);
}

TEST_P (StoreOutageRetention, SameTokenExactMatchSpotWide)
{
    registered_owner (true, true, -10);
}

TEST_P (StoreOutageRetention, SameTokenExactMissingIsObjectLocalPerActor)
{
    registered_owner (false, true, -11);
}

TEST_P (StoreOutageRetention, SameTokenExactMissingIsObjectLocalSpotWide)
{
    registered_owner (true, true, -11);
}

TEST_P (StoreOutageRetention, SameTokenMemberWaitsForSpotPerActor)
{
    registered_owner (false, true, -12);
}

TEST_P (StoreOutageRetention, SameTokenMemberWaitsForSpotSpotWide)
{
    registered_owner (true, true, -12);
}

TEST_P (StoreOutageRetention, RuntimeStopPreventsQueuedOwnerCleanupPerActor)
{
    registered_owner (false, true, -3);
}
TEST_P (StoreOutageRetention, RuntimeStopPreventsQueuedOwnerCleanupSpotWide)
{
    registered_owner (true, true, -3);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerPerActorRetention)
{
    registered_owner (false, true);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerSpotWideRetention)
{
    registered_owner (true, true);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerPerActorDefault)
{
    registered_owner (false, false);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerSpotWideDefault)
{
    registered_owner (true, false);
}

TEST_P (StoreOutageRetention, ReviewMembershipCasRefreshesPreservedFence)
{
    registered_owner (false, true, -24);
}
TEST_P (StoreOutageRetention, ReviewMembershipCasDoesNotAcceptSupersedingFence)
{
    registered_owner (false, true, -25);
}

TEST_P (StoreOutageRetention, ReviewAdoptionExactFenceMatches)
{
    registered_owner (false, true, -20);
}
TEST_P (StoreOutageRetention, ReviewAdoptionExactFenceMismatchIsObjectLocal)
{
    registered_owner (false, true, -21);
}
TEST_P (StoreOutageRetention, ReviewDuplicateTerminalExactFenceMatches)
{
    registered_owner (false, true, -22);
}
TEST_P (StoreOutageRetention, ReviewDuplicateTerminalExactFenceMismatchIsObjectLocal)
{
    registered_owner (false, true, -23);
}

TEST_P (StoreOutageRetention, ReviewHeartbeatDoesNotWaitOrRestartExactRead)
{
    registered_owner (false, true, -14);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryPreserveCas)
{
    registered_owner (false, true, -55);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryExactRead)
{
    registered_owner (false, true, -56);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryReadAfterCutoff)
{
    registered_owner (false, true, -57);
}

TEST_P (StoreOutageRetention, ReviewCutoffTerminatesReadBeforeEpisodeRelease)
{
    registered_owner (false, true, -15);
}
TEST_P (StoreOutageRetention, ReviewOnlyReadErrorsRetryOnNextRenew)
{
    registered_owner (false, true, -16);
}

TEST_P (StoreOutageRetention, ContendedOwnerSnapshotCannotDelayCutoff)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "contended-snapshot" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
        auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("contended");
        node->worker_executor = scheduler;
        detail::spot_node_runtime_t owner (node);
        location_lifecycle_t lifecycle (runtime);
        owner.bind_location_lifecycle (lifecycle);
        auto context = std::make_shared<detail::spot_context_state_t> ();
        context->node = node;
        context->lane_owner = node;
        context->serial_executor = scheduler;
        context->spot_id = spot_id_t ("contended-spot");
        context->mesh_name = "contended";
        context->node_rid = node_rid_t::from_string ("contended-node");
        node->spot_contexts_by_id.emplace ("contended-spot",
                                           detail::spot_context_access_t::create (context));
        const auto confirmed_at = clock_now.load ();
        auto writer = owner
                        .prepare_store_ready ({stateful::object_kind_t::user_spot, "contended-spot",
                                               1, 1, "contended", "contended-node"})
                        .result ()
                        .value ();
        ASSERT_TRUE (writer);
        auto gate = context->ensure_spot_serial_executor ()->spot_queue ();
        const auto cutoff = retained ? confirmed_at + 40s : confirmed_at + 25s;
        std::latch snapshot_entered (1), snapshot_released (1);
        auto source = std::async (std::launch::async, [&] {
            node->lane.run_sync ([&] {
                snapshot_entered.count_down ();
                snapshot_released.wait ();
            });
        });
        snapshot_entered.wait ();
        writer (confirmed_at);
        advance (5s);
        auto opening =
          std::async (std::launch::async, [&] { lease_access::communication_failure (runtime); });
        opening.get ();
        move_to (cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        int calls = 0;
        EXPECT_TRUE (gate->try_post ("while-snapshot-owner-is-contended", [&] { ++calls; }));
        scheduler->run_all ();
        snapshot_released.count_down ();
        source.get ();
        EXPECT_EQ (0, calls);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_TRUE (episode->discarded.load ());
        gate->cancel_pending ();
        scheduler->run_all ();
        node->spot_contexts_by_id.clear ();
        context->node.reset ();
    }
}

TEST_P (StoreOutageRetention, ReadyPolicyAndLateWriterRetainTheirOwnedPublication)
{
    auto runtime = std::make_unique<location_runtime_t> (store (), options, "policy-lifetime");
    lease_access::clock (*runtime, [&] { return clock_now.load (); });
    claim (*runtime);
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy (runtime->execution_policy ());
    auto writer = gate->ready_confirmation ();
    ASSERT_TRUE (writer);
    writer (clock_now.load ());
    advance (5s);
    lease_access::communication_failure (*runtime);
    const auto cutoff = lease_access::snapshot (*runtime)->outage->cutoff;
    int calls = 0;
    ASSERT_TRUE (gate->try_post ("queued-before-location-owner-destruction", [&] { ++calls; }));
    runtime.reset ();
    move_to (cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (gate->try_post ("after-location-owner-destruction", [&] { ++calls; }));
    const auto rejected = gate->try_post_cancellable_async (
      "after-owner-cancellable", [] (auto complete) { complete ([] {}); }, [] {});
    ASSERT_FALSE (rejected);
    EXPECT_EQ (framework_error_kind_t::shutting_down, rejected.error_kind ());
    scheduler.run_all ();
    EXPECT_EQ (0, calls);
    gate->cancel_pending ();
    scheduler.run_all ();
    gate.reset ();
    writer (clock_now.load ());
}

TEST_P (StoreOutageRetention, CutoffAndScheduledLatchDoNotChangeAdmission)
{
    for (bool enabled : {false, true}) {
        options.store_outage_retention = enabled;
        location_runtime_t runtime (store (), options, enabled ? "enabled-owner" : "default-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto initial = lease_access::snapshot (runtime);
        advance (5s);
        lease_access::communication_failure (runtime);
        auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_EQ (clock_now.load (), episode->observed_at);
        EXPECT_EQ (enabled ? initial->last_successful_lease->started_at + 40s
                           : *initial->admission_deadline,
                   episode->cutoff);
        EXPECT_EQ (initial->admission_deadline,
                   lease_access::snapshot (runtime)->admission_deadline);
        move_to (episode->cutoff - 1ms);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_FALSE (episode->discarded.load ());
        EXPECT_EQ (lease_access::usable_at (runtime, clock_now.load ()),
                   clock_now.load () < *initial->admission_deadline);
        advance (1ms);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_TRUE (episode->discarded.load ());
        EXPECT_EQ (initial->admission_deadline,
                   lease_access::snapshot (runtime)->admission_deadline);
        EXPECT_TRUE (lease_access::outage_discarded (runtime));
    }
}

TEST_P (StoreOutageRetention, NewClaimAndLateRenewCannotChangeOriginalEpisode)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto original = lease_access::snapshot (runtime);
    advance (5s);
    lease_access::communication_failure (runtime);
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    auto token = *original->token;
    const auto store_now = *original->renewed_at;
    ++token.lease_generation;
    lease_access::accept (runtime, token, store_now + 30s, store_now, clock_now.load (), false);
    advance (1s);
    lease_access::accept (runtime, *original->token, store_now + 30s, store_now,
                          original->last_successful_lease->started_at + 1s, true);
    EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
    EXPECT_EQ (original->last_successful_lease->started_at + 40s, episode->cutoff);
    move_to (episode->cutoff);
    // Even a recovery response delivered before the due callback cannot erase the latch.
    lease_access::accept (runtime, *original->token, store_now + 30s, store_now,
                          clock_now.load () - 1s, true, episode);
    EXPECT_TRUE (episode->discarded.load ());
    const auto retired = lease_access::snapshot (runtime);
    EXPECT_FALSE (retired->outage);
    ASSERT_EQ (1u, retired->discarded_outages.size ());
    EXPECT_EQ (episode, retired->discarded_outages.front ().lock ());
    EXPECT_EQ (original->last_successful_lease->started_at + 40s, episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ClaimAdvancesBaselineAndLateSuccessCannotMoveItBack)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    advance (5s);
    auto new_token = *initial->token;
    ++new_token.lease_generation;
    const auto claim_started_at = clock_now.load ();
    const auto store_now = *initial->renewed_at;
    lease_access::accept (runtime, new_token, store_now + 30s, store_now, claim_started_at, false);
    EXPECT_EQ (claim_started_at,
               lease_access::snapshot (runtime)->last_successful_lease->started_at);

    lease_access::accept (runtime, new_token, store_now + 30s, store_now,
                          initial->last_successful_lease->started_at, true);
    EXPECT_EQ (claim_started_at,
               lease_access::snapshot (runtime)->last_successful_lease->started_at);
    advance (1s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (claim_started_at + 40s, episode->cutoff);
}

TEST_P (StoreOutageRetention, TimelySameOwnerRenewClosesEpisodeAndCancelsLatch)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    lease_access::communication_failure (runtime);
    auto snapshot = lease_access::snapshot (runtime);
    auto episode = snapshot->outage;
    ASSERT_TRUE (episode);
    advance (1s);
    lease_access::accept (runtime, *snapshot->token, *snapshot->renewed_at + 30s,
                          *snapshot->renewed_at, clock_now.load (), true, episode);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (episode->discarded.load ());
    // A second episode uses the last successful renewal request, not the initial claim.
    lease_access::communication_failure (runtime);
    EXPECT_EQ (snapshot->outage->observed_at + 1s + 40s,
               lease_access::snapshot (runtime)->outage->cutoff);
}

TEST_P (StoreOutageRetention, ProviderErrorsOpenEpisodeAndActualRenewClosesIt)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    provider.fail = memory.fail = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (clock_now.load (), episode->observed_at);
    provider.fail = memory.fail = false;
    advance (1s);
    lease_access::heartbeat (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ConfirmationReadDoesNotRenewRetentionBaseline)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    auto initial = lease_access::snapshot (runtime);
    advance (5s);
    provider.fail_next_read = memory.fail_renew = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto snapshot = lease_access::snapshot (runtime);
    ASSERT_TRUE (snapshot->outage);
    EXPECT_TRUE (snapshot->healthy);
    EXPECT_EQ (initial->last_successful_lease->started_at,
               snapshot->last_successful_lease->started_at);
    EXPECT_EQ (initial->last_successful_lease->started_at + 40s, snapshot->outage->cutoff);
    move_to (snapshot->outage->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (snapshot->outage->discarded.load ());
}

TEST_P (StoreOutageRetention, RenewTimeoutOpensEpisode)
{
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    provider.hold_read = memory.hold_renew = true;
    lease_access::heartbeat (runtime, clock_now.load (), [&] {
        advance (options.owner_lease_renew_timeout);
        provider.resume.complete (result_t<void>::success ());
        memory.resume.complete (result_t<void>::success ());
    });
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (clock_now.load (), episode->observed_at);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ReadErrorAndReadTimeoutOpenEpisode)
{
    for (bool timeout : {false, true}) {
        location_runtime_t runtime (store (), options, timeout ? "timeout-owner" : "error-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        advance (5s);
        if (timeout) {
            provider.hold_read = memory.hold_read = true;
        } else {
            provider.fail = memory.fail = true;
        }
        lease_access::confirm (runtime, clock_now.load (), [&] {
            if (timeout) {
                advance (options.owner_lease_renew_timeout);
                provider.resume.complete (result_t<void>::success ());
                memory.resume.complete (result_t<void>::success ());
            }
        });
        auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_EQ (clock_now.load (), episode->observed_at);
        provider.hold_read = memory.hold_read = false;
        provider.fail = memory.fail = false;
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_TRUE (episode->discarded.load ());
    }
}

TEST_P (StoreOutageRetention, ConflictMissingAndStaleDoNotOpenEpisode)
{
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    location_runtime_t conflict (store (), options, runtime.owner_id ());
    lease_access::clock (conflict, [&] { return clock_now.load (); });
    lease_access::heartbeat (conflict, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (conflict)->outage);
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    lease_access::confirm (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    lease_access::heartbeat (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossPerActorDefault)
{
    registered_owner (false, false, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossPerActorRetention)
{
    registered_owner (false, true, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossSpotWideDefault)
{
    registered_owner (true, false, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossSpotWideRetention)
{
    registered_owner (true, true, 1);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossPerActorDefault)
{
    registered_owner (false, false, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossPerActorRetention)
{
    registered_owner (false, true, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossSpotWideDefault)
{
    registered_owner (true, false, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossSpotWideRetention)
{
    registered_owner (true, true, 2);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossPerActorDefault)
{
    registered_owner (false, false, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossPerActorRetention)
{
    registered_owner (false, true, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossSpotWideDefault)
{
    registered_owner (true, false, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossSpotWideRetention)
{
    registered_owner (true, true, 3);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossPerActorDefault)
{
    registered_owner (false, false, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossPerActorRetention)
{
    registered_owner (false, true, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossSpotWideDefault)
{
    registered_owner (true, false, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossSpotWideRetention)
{
    registered_owner (true, true, 4);
}

TEST_P (StoreOutageRetention, PendingStopAndOwnershipLossHaveDifferentResults)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true})
        for (auto status : {status_t::stopped, status_t::ownership_lost}) {
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            auto gate = shared ? spot : actor;
            int handlers = 0, completed = 0;
            serial_work_options_t pending;
            pending.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (status == status_t::stopped ? framework_error_kind_t::shutting_down
                                                       : framework_error_kind_t::not_found,
                           result.error_kind ());
                ++completed;
            };
            ASSERT_TRUE (gate->try_post ("pending", [&] { ++handlers; }, pending));
            gate->cancel_pending (status);
            scheduler.run_all ();
            EXPECT_EQ (0, handlers);
            EXPECT_EQ (1, completed);
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
        }
}

TEST_P (StoreOutageRetention, TerminalCauseSurvivesParentDiscardAndLaterPolicy)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true})
        for (bool stopped_first : {false, true}) {
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            int handlers = 0, completed = 0;
            serial_work_options_t pending;
            pending.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (stopped_first ? framework_error_kind_t::shutting_down
                                         : framework_error_kind_t::not_found,
                           result.error_kind ());
                ++completed;
            };
            ASSERT_TRUE (actor->try_post ("pending", [&] { ++handlers; }, pending));
            if (stopped_first)
                actor->cancel_pending ();
            ASSERT_TRUE (spot->fence_execution ());
            // A subsequent retention/Ready publication cannot replace the terminal fence.
            spot->set_execution_policy ([] { return true; });
            EXPECT_EQ (status_t::ownership_lost, spot->execution_status ());
            EXPECT_FALSE (spot->execution_allowed ());
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
            EXPECT_EQ (0, handlers);
            EXPECT_EQ (1, completed);
        }
}

TEST_P (StoreOutageRetention, RecoveryMustWaitForNormalExpiryBeforeNewClaim)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "normal-expiry-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto old_token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    ASSERT_LT (clock_now.load (), initial->last_successful_lease->started_at
                                    + options.owner_lease_ttl + options.owner_lease_fencing_margin);
    store ().release_owner_lease (old_token).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    // Approved D17: Missing alone cannot authorize a claim before lifetime + margin.
    const auto after = lease_access::snapshot (runtime)->token;
    ASSERT_TRUE (after);
    EXPECT_EQ (old_token.lease_generation, after->lease_generation);
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryCanClaimAfterNormalExpiryAndExactMissing)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "expired-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto old_token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin);
    store ().release_owner_lease (old_token).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto after = lease_access::snapshot (runtime)->token;
    ASSERT_TRUE (after);
    EXPECT_NE (after->lease_generation, old_token.lease_generation);
}


TEST_P (StoreOutageRetention, RecoveryMissingBeforeExpiryBoundaryKeepsOriginalToken)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "boundary-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    store ().release_owner_lease (token).result ().value ();
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin - 1ms);
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
    EXPECT_FALSE (episode->discarded.load ());
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryDifferentTokenDiscardsWithoutAdoptingIt)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "changed-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    store ().release_owner_lease (token).result ().value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      store ().claim_owner_lease (token.owner_id, options.owner_lease_ttl).result ().value ());
    ASSERT_NE (token.lease_generation, replacement.token.lease_generation);
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (episode->discarded.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->healthy);
    const auto exact =
      std::get<owner_lease_found_t> (store ().read_owner_lease (token.owner_id).result ().value ());
    EXPECT_EQ (replacement.token.lease_generation, exact.token.lease_generation);
}

TEST_P (StoreOutageRetention, RecoveryFirstCommunicationFailureDiscardsExactDifferentToken)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "changed-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    store ().release_owner_lease (token).result ().value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      store ().claim_owner_lease (token.owner_id, options.owner_lease_ttl).result ().value ());
    ASSERT_NE (token.lease_generation, replacement.token.lease_generation);
    provider.fail_next_read = memory.fail_renew = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    provider.fail_next_read = memory.fail_renew = false;
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (episode->discarded.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->healthy);
    const auto exact =
      std::get<owner_lease_found_t> (store ().read_owner_lease (token.owner_id).result ().value ());
    EXPECT_EQ (replacement.token.lease_generation, exact.token.lease_generation);
}

TEST_P (StoreOutageRetention, RecoveryFailedExactReadDoesNotAuthorizeClaim)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "read-failure-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    store ().release_owner_lease (token).result ().value ();
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin);
    provider.fail = memory.fail = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    provider.fail = memory.fail = false;
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_NE (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeStillClaimsImmediately)
{
    location_runtime_t runtime (store (), options, "outside-episode-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    advance (5s);
    store ().release_owner_lease (token).result ().value ();
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_NE (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
}

TEST_P (StoreOutageRetention, StaleRenewStartedBeforeOutageCannotClaimEarly)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "pending-stale-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    advance (5s);
    provider.hold_read = memory.hold_renew = true;
    lease_access::heartbeat (runtime, clock_now.load (), [&] {
        lease_access::communication_failure (runtime);
        provider.hold_read = memory.hold_renew = false;
        provider.resume.complete (result_t<void>::success ());
        memory.resume.complete (result_t<void>::success ());
    });
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    ASSERT_TRUE (lease_access::snapshot (runtime)->outage);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage->discarded.load ());
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryExactLeaseMustBeExpiredAsWellAsNormalLifetimeElapsed)
{
    for (bool expired : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (store (), options, expired ? "expired-exact" : "live-exact");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        const auto initial = lease_access::snapshot (runtime);
        const auto token = *initial->token;
        auto exact = std::get<owner_lease_found_t> (
          store ().read_owner_lease (token.owner_id).result ().value ());
        const auto lifetime = expired ? 0ms : 1ms;
        exact.lease_expires_at = exact.store_now + lifetime;
        std::optional<store_found_t> raw;
        if (GetParam ()) {
            raw = std::get<store_found_t> (
              provider.inner.read ({std::string ("owner-lease") + '\0' + token.owner_id})
                .result ()
                .value ());
            raw->value.expires_at = raw->value.store_now + lifetime;
        }
        advance (5s);
        lease_access::communication_failure (runtime);
        store ().release_owner_lease (token).result ().value ();
        move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
                 + options.owner_lease_fencing_margin);
        if (GetParam ())
            provider.injected_owner_reads = {store_missing_t{exact.store_now}, *raw};
        else
            memory.injected_owner_reads = {exact};
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_EQ (expired, token.lease_generation
                              != lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_TRUE (provider.injected_owner_reads.empty ());
        EXPECT_TRUE (memory.injected_owner_reads.empty ());
        const auto actual = store ().read_owner_lease (token.owner_id).result ().value ();
        EXPECT_EQ (!expired, std::holds_alternative<owner_lease_missing_t> (actual));
    }
}

TEST_P (StoreOutageRetention, OrdinaryPublicationTransfersCallbackWithoutStart)
{
    for (const bool confirmed : {false, true}) {
        location_runtime_t runtime (store (), options,
                                    confirmed ? "callback-confirmed" : "callback-unconfirmed");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto builder = test::runtime_failure_builder ();
        handler_registry_t handlers;
        serializer_registry_t serializers;
        auto failures = std::make_shared<runtime_failure_collector_t> ();
        unsigned publications = 0;
        location_auto_connect_host_service_t service (
          failures, builder.message_bus (), {}, handlers, serializers,
          [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
          [&] (auto intent, auto, auto) {
              ++publications;
              EXPECT_EQ (location_write_intent_t::renew, intent);
              return task_t<bool> (result_t<bool>::success (confirmed));
          });
        EXPECT_EQ (confirmed,
                   location_auto_connect_test_access_t::publish_without_start (service, runtime)
                     .result ()
                     .value ());
        EXPECT_EQ (1u, publications);
        EXPECT_EQ (confirmed,
                   runtime
                     .republish_descriptors (location_write_intent_t::renew, {},
                                             clock_now.load () + options.owner_lease_renew_timeout)
                     .result ()
                     .value ());
        EXPECT_EQ (2u, publications);
        EXPECT_EQ (confirmed,
                   location_auto_connect_test_access_t::publish_without_start (service, runtime)
                     .result ()
                     .value ());
        EXPECT_EQ (3u, publications);
        service.stop ();
        EXPECT_FALSE (
          runtime
            .republish_descriptors (location_write_intent_t::renew, {},
                                    clock_now.load () + options.owner_lease_renew_timeout)
            .result ()
            .value ());
        EXPECT_EQ (3u, publications);
        failures->rethrow_if_failed ();
    }
}

TEST_P (StoreOutageRetention, FirstPublicationWaitsForRenewalObservation)
{
    location_runtime_t runtime (store (), options, "first-publication-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    serializer_registry_t serializers;
    mesh_node_host_service_t host (std::make_shared<runtime_failure_collector_t> (), {},
                                   serializers);
    mesh_node_host_service_test_access_t::configure (
      host, std::shared_ptr<location_repository_t> (&store (), [] (auto *) {}), runtime);
    lease_access::publisher (runtime, [&] (auto intent, auto cancellation, auto deadline) {
        return host.republish_after_store_recovery (intent, cancellation, deadline);
    });
    const auto observed = runtime.owner_lease_observation ();
    const auto [unchanged, ordinary] = runtime.descriptor_publication_observation (observed);
    EXPECT_EQ (observed, unchanged);
    EXPECT_FALSE (
      runtime
        .republish_descriptors (ordinary, {}, clock_now.load () + options.owner_lease_renew_timeout)
        .result ()
        .value ());
    EXPECT_TRUE (store ().list_mesh_nodes ("publication-mesh").result ().value ().items.empty ());
    advance (1ms);
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto [renewed, retry] = runtime.descriptor_publication_observation (observed);
    EXPECT_NE (observed, renewed);
    EXPECT_TRUE (
      runtime
        .republish_descriptors (retry, {}, clock_now.load () + options.owner_lease_renew_timeout)
        .result ()
        .value ());
    EXPECT_EQ (1u, store ().list_mesh_nodes ("publication-mesh").result ().value ().items.size ());
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    EXPECT_EQ (0u, provider.preserve_writes.load ());
    EXPECT_EQ (0u, memory.preserve_writes.load ());
}

TEST_P (StoreOutageRetention, InitialClaimAndOrdinaryPublicationKeepStartupOwnership)
{
    for (const bool confirmed : {false, true}) {
        location_runtime_t runtime (store (), options,
                                    confirmed ? "startup-confirmed" : "startup-waiting");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        unsigned publications = 0, recoveries = 0;
        lease_access::publisher (runtime, [&] (auto intent, auto, auto) {
            ++publications;
            EXPECT_EQ (location_write_intent_t::renew, intent);
            return task_t<bool> (result_t<bool>::success (confirmed));
        });
        runtime
          .on_execution_discard ([] (auto) {}, {},
                                 [&] (location_runtime_t::execution_recovery_t, monotonic_time) {
                                     ++recoveries;
                                     return task_t<void> (result_t<void>::success ());
                                 })
          .result ()
          .value ();
        claim (runtime);
        advance (1ms);
        lease_access::heartbeat (runtime, clock_now.load ());
        const auto previous = *lease_access::snapshot (runtime)->token;
        store ().release_owner_lease (previous).result ().value ();
        advance (1ms);
        lease_access::heartbeat (runtime, clock_now.load ());
        EXPECT_NE (previous.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_EQ (0u, publications);
        EXPECT_EQ (0u, recoveries);
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (confirmed,
                   runtime
                     .republish_descriptors (location_write_intent_t::renew, {},
                                             clock_now.load () + options.owner_lease_renew_timeout)
                     .result ()
                     .value ());
        EXPECT_EQ (1u, publications);
        EXPECT_EQ (0u, recoveries);
        EXPECT_TRUE (runtime.owner_lease_healthy ());
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (0u, provider.preserve_writes.load ());
        EXPECT_EQ (0u, memory.preserve_writes.load ());
    }
}

TEST_P (StoreOutageRetention, StaleLeaseKeepsObjectsAndOriginalEpisodeUntilCutoff)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options, "stale-owner-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy (runtime.execution_policy ());
        gate->confirm_execution_ready (clock_now.load ());
        int discarded = 0;
        runtime
          .on_execution_discard ([&] (auto allowed) {
              if (allowed (*gate) != serial_execution_queue_t::execution_status_t::allowed) {
                  ++discarded;
                  gate->cancel_pending ();
              }
          })
          .result ()
          .value ();
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        const auto old_token = *lease_access::snapshot (runtime)->token;
        store ().release_owner_lease (old_token).result ().value ();
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_EQ (old_token.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
          store ().read_owner_lease (old_token.owner_id).result ().value ()));
        EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (0, discarded);
        int handlers = 0;
        ASSERT_TRUE (gate->try_post ("after-stale-before-expiry", [&] { ++handlers; }));
        scheduler.run_all ();
        EXPECT_EQ (1, handlers);
        const auto normal_expiry =
          lease_access::snapshot (runtime)->last_successful_lease->started_at
          + options.owner_lease_ttl + options.owner_lease_fencing_margin;
        // The default cutoff precedes normal expiry; retained execution lasts until T.
        if (episode->cutoff <= normal_expiry) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            scheduler.run_all ();
            EXPECT_EQ (1, discarded);
        }
        move_to (normal_expiry);
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_NE (old_token.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        if (episode->discarded.load ()) {
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                       gate->execution_status ());
        } else
            EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (retained ? 0 : 1, discarded);
        // A renew of the newly claimed token cannot extend the old episode.
        advance (1ms);
        lease_access::heartbeat (runtime, clock_now.load ());
        if (episode->discarded.load ()) {
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                       gate->execution_status ());
        } else
            EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        if (clock_now.load () < episode->cutoff)
            move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        EXPECT_EQ (1, discarded);
        EXPECT_TRUE (gate->closed ());
        EXPECT_FALSE (gate->try_post ("after-cutoff", [&] { ++handlers; }));
    }
}

TEST_P (StoreOutageRetention, CachedBoundSpotRoutePerActorDefault)
{
    registered_owner (false, false, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRoutePerActorRetention)
{
    registered_owner (false, true, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRouteSpotWideDefault)
{
    registered_owner (true, false, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRouteSpotWideRetention)
{
    registered_owner (true, true, -1);
}

TEST (ZLinkFrameworkLocationRuntime, ParentRecoveryNotifiesChildOnceForThousandEnqueues)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto parent = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto child = std::make_shared<serial_execution_queue_t> (scheduler);
        parent->set_execution_policy ([] { return true; });
        child->set_execution_policy ([] { return true; });
        child->attach_spot_gate (parent);
        const auto at = monotonic_time{std::chrono::hours (1000000)};
        parent->confirm_execution_ready (at);
        child->confirm_execution_ready (at);
        scheduler.run_all ();
        task_completion_source_t<serial_execution_queue_t::execution_operation_t> pending;
        unsigned calls = 0;
        for (unsigned i = 0; i != 1000; ++i)
            ASSERT_TRUE (child->try_post ("retained", [&] { ++calls; }));
        parent->set_execution_operation (pending.task ());
        EXPECT_FALSE (child->try_post ("new-while-paused", [] {}));
        scheduler.run_all ();
        EXPECT_EQ (0u, calls);
        pending.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
          {serial_execution_queue_t::execution_status_t::allowed, at + 1s}));
        // A PerActor Spot has its own control turn before the child mailbox drain.
        // Observe the child's one resume only after that parent control turn completes.
        if (!shared) {
            ASSERT_TRUE (scheduler.run_one ());
            EXPECT_EQ (0u, calls);
        }
        EXPECT_EQ (1u, scheduler.pending_work ());
        scheduler.run_all ();
        EXPECT_EQ (1000u, calls);
        EXPECT_EQ (0u, scheduler.pending_work ());
        child->cancel_pending ();
        parent->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST (ZLinkFrameworkLocationRuntime, RecoveryOperationPublishesAllowedLostAndReadFailure)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    using operation_t = serial_execution_queue_t::execution_operation_t;
    for (auto terminal : {status_t::allowed, status_t::ownership_lost, status_t::stopped}) {
        test::manual_internal_scheduler_t scheduler;
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy ([] { return true; });
        const auto at = monotonic_time{std::chrono::hours (1000000)};
        gate->confirm_execution_ready (at);
        scheduler.run_all ();
        EXPECT_EQ (status_t::allowed, gate->execution_status ());
        task_completion_source_t<operation_t> superseded;
        gate->set_execution_operation (superseded.task ());
        task_completion_source_t<operation_t> pending;
        gate->set_execution_operation (pending.task ());
        superseded.complete (result_t<operation_t>::success ({status_t::allowed, at + 3s}));
        EXPECT_EQ (status_t::stopped, gate->execution_status ());
        EXPECT_FALSE (gate->execution_allowed ());
        // A paused object retains eligibility for the original episode's T latch.
        ASSERT_TRUE (gate->execution_ready_at ());
        EXPECT_LT (*gate->execution_ready_at (), at + 1s);
        if (terminal == status_t::stopped)
            pending.complete (result_t<operation_t>::failure (framework_error_kind_t::unavailable,
                                                              "injected exact read failure"));
        else
            pending.complete (result_t<operation_t>::success ({terminal, at + 2s}));
        scheduler.run_all ();
        EXPECT_EQ (terminal, gate->execution_status ());
        EXPECT_EQ (terminal == status_t::allowed, gate->execution_allowed ());
        EXPECT_EQ (0u, tests::count_allocations ([&] {
                       for (unsigned i = 0; i != 1000; ++i) {
                           (void) gate->execution_status ();
                           (void) gate->execution_allowed ();
                           (void) gate->execution_ready_at ();
                       }
                   }));
        gate->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST (ZLinkFrameworkLocationRuntime, LocationRecoveryCannotRestoreInvalidatedReadyProof)
{
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy ([] { return true; });
    const auto at = monotonic_time{std::chrono::hours (1000000)};
    gate->confirm_execution_ready (at);
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> pending;
    gate->set_execution_operation (pending.task ());
    gate->invalidate_execution_ready ();
    pending.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, at + 1s}));
    scheduler.run_all ();
    EXPECT_FALSE (gate->execution_ready_at ());
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> stale;
    gate->set_execution_operation (stale.task ());
    stale.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, at + 2s}));
    scheduler.run_all ();
    EXPECT_FALSE (gate->execution_ready_at ());
    gate->cancel_pending ();
    scheduler.run_all ();
}

TEST (ZLinkFrameworkLocationRuntime, PreservedRegistryDoesNotParticipateInNormalTurnAdmission)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    int clocks = 0;
    const auto at = monotonic_time{std::chrono::hours (1000000)};
    lease_access::clock (runtime, [&] {
        ++clocks;
        return at;
    });
    auto object_owner = std::make_shared<stateful::stateful_object_runtime_t> ();
    auto &objects = *object_owner;
    test::manual_internal_scheduler_t scheduler;
    std::vector<std::shared_ptr<serial_execution_queue_t>> gates;
    for (unsigned i = 0; i != 5000; ++i) {
        stateful::object_ref_t object{
          stateful::object_kind_t::actor, "preserved-" + std::to_string (i), 1, 1, "mesh", "node"};
        const auto created =
          objects.begin_reserved_object_async (object, "player", {}).result ().value ();
        ASSERT_EQ (stateful::stateful_error_t::none, created.error);
        ASSERT_EQ (stateful::stateful_error_t::none, objects.commit_create (created.attempt));
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy (runtime.execution_policy ());
        objects.bind_execution_gate (object, gate).result ().value ();
        authority_snapshot_t snapshot;
        snapshot.store_version = "confirmed-" + std::to_string (i);
        snapshot.object_generation = snapshot.authority_owner_generation = 1;
        objects.confirm_location_ready (object, snapshot, gate->ready_confirmation (), at)
          .result ()
          .value ();
        gates.push_back (std::move (gate));
    }
    unsigned enumerations = 0, validations = 0;
    const auto recover = [&] {
        ++enumerations;
        return stateful::stateful_object_runtime_t::recover_location_execution (
          object_owner,
          [&] (authority_key_t, authority_snapshot_t snapshot,
               std::function<task_t<void> (authority_snapshot_t)>) {
              ++validations;
              EXPECT_TRUE (snapshot.store_version.starts_with ("confirmed-"));
              return task_t<serial_execution_queue_t::execution_operation_t> (
                result_t<serial_execution_queue_t::execution_operation_t>::success (
                  {serial_execution_queue_t::execution_status_t::allowed, at + 2s}));
          },
          at + 1s);
    };
    recover ().result ().value ();
    scheduler.run_all ();
    ASSERT_EQ (5000u, validations);
    ASSERT_EQ (1u, enumerations);
    clocks = 0;
    std::size_t allowed = 0;
    const auto allocations = tests::count_allocations ([&] {
        for (unsigned turn = 0; turn != 32; ++turn)
            for (const auto &gate : gates)
                allowed += gate->execution_allowed ();
    });
    EXPECT_EQ (160000u, allowed);
    EXPECT_EQ (0u, allocations);
    EXPECT_EQ (0, clocks);
    EXPECT_EQ (5000u, validations);
    EXPECT_EQ (1u, enumerations);
    recover ().result ().value ();
    scheduler.run_all ();
    EXPECT_EQ (2u, enumerations);
    EXPECT_EQ (5000u, validations); // The same episode cannot pause recovered objects again.
    // Positive control: the same counter must detect a deliberately allocated object.
    EXPECT_GT (tests::count_allocations ([] {
                   auto value = std::make_shared<int> (1);
                   EXPECT_EQ (1, *value);
               }),
               0u);
    for (auto &gate : gates)
        gate->cancel_pending ();
    scheduler.run_all ();
}

INSTANTIATE_TEST_SUITE_P (BothRepositories, StoreOutageRetention, ::testing::Bool ());

TEST (ZLinkFrameworkLocationRuntime, StoreOutageRetentionIsStartupCopyWithFalseDefault)
{
    location_options_t positional{7s};
    EXPECT_EQ (7s, positional.owner_lease_renew_interval);
    EXPECT_FALSE (positional.store_outage_retention);
    location_options_t defaults;
    EXPECT_FALSE (defaults.store_outage_retention);
    detail::validate_location_options (defaults);
    app_t app;
    auto &builder = app.add_zlink_framework ();
    EXPECT_FALSE (builder.location_options ().store_outage_retention);
    builder.configure_locations ().store_outage_retention = true;
    auto options = builder.location_options ();
    detail::validate_location_options (options);
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, options, "startup-owner");
    lease_access::clock (runtime, [] { return monotonic_time{std::chrono::hours (1000000)}; });
    lease_access::heartbeat (runtime, monotonic_time{std::chrono::hours (1000000)});
    options.store_outage_retention = false;
    builder.configure_locations ().store_outage_retention = false;
    EXPECT_TRUE (runtime.options ().store_outage_retention);
    static_assert (std::is_const_v<std::remove_reference_t<decltype (runtime.options ())>>);
}
} // namespace
} // namespace zlink::framework::runtime
