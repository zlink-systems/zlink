/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/execution_cases.hpp"
#include "../support/execution_settings.hpp"
#include "../support/execution_wait.hpp"
#include "../support/execution_serial_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/streams/session_serial_executor.hpp"
#include "runtime/fanout/fanout_location_runtime.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

using zlink::framework::test::begin_reserved_object;
using zlink::framework::test::scope_exit_t;

namespace zlink::framework::runtime
{
struct state_lane_test_access_t
{
    static bool has_pending (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return !lane._mailbox.empty ();
    }
};
}

namespace zlink::framework::runtime::fanout
{
struct fanout_location_runtime_test_access_t
{
    static state_lane_t &lane (fanout_location_runtime_t &runtime) { return runtime._lane; }
    static void pump (fanout_location_runtime_t &runtime) { runtime.pump (); }
    static void run (fanout_location_runtime_t &runtime) { runtime.run (); }
    static void request_stop (fanout_location_runtime_t &runtime) { runtime.request_stop (); }
};
}

namespace zlink::framework::test::execution
{
bool verify_session_control_lifecycle_priority ()
{
    using namespace zlink::framework::runtime;
    serial_executor_test_fixture_t fixture (serial_lane_policy_t::per_actor_spot ());
    session_serial_executor_t session (fixture.worker);
    serial_test_blocker_t first;
    std::vector<int> order;
    if (!session.execute_application (first.work ()) || !first.wait_for_entry ())
        return false;
    const auto append = [&] (int value) {
        return [&, value] (auto complete) {
            order.push_back (value);
            complete ([] {});
        };
    };
    const bool accepted = session.execute_application (append (3))
                          && session.execute_control (append (1))
                          && session.execute_control (append (2));
    first.release ();
    session.drain ();
    return accepted && order == std::vector<int>{1, 2, 3};
}

bool verify_fanout_snapshot_state_owner ()
{
    using namespace zlink::framework;
    completion_owner_t completion_owner;
    runtime::configure_handler_coroutine_executor (handler_coroutine_worker_count);
    runtime::install_host_context_hooks ();
    scope_exit_t shutdown ([] { runtime::shutdown_handler_coroutine_executor (); });
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    handler_registry_t handlers;
    runtime::in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    runtime::location_runtime_t locations (store);
    channel_snapshot_t channel;
    channel.name = "state-owner";
    channel.subscriber.enabled = true;
    channel.subscriber.discovery = true;
    auto executor = std::make_shared<runtime::offload_executor_t> ();
    std::atomic_bool on_lane{true};
    auto builder = zlink::framework::test::runtime_failure_builder ();
    runtime::fanout::fanout_location_runtime_t fanout (
      builder.message_bus (), {channel}, locations, store, store, provider, serializers, handlers,
      [&] {
          if (!runtime::state_lane_t::current ())
              on_lane.store (false);
          return framework_runtime_state_t::serving;
      },
      executor);
    const auto initial = fanout.snapshot (channel.name);
    std::vector<std::future<fanout_channel_snapshot_t>> reads;
    for (int i = 0; i < 8; ++i)
        reads.push_back (
          std::async (std::launch::async, [&] { return fanout.snapshot (channel.name); }));
    bool same_sequence = initial.sequence == 1;
    for (auto &read : reads)
        same_sequence = read.get ().sequence == initial.sequence && same_sequence;
    std::promise<fanout_channel_snapshot_t> observed;
    auto delivered = observed.get_future ();
    auto observation = fanout.observe (
      channel.name, 4, [&] (const auto &status) { observed.set_value (status.status); });
    const bool ready = delivered.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    const bool coherent = ready && delivered.get ().sequence == initial.sequence;
    observation->close ();
    auto &lane = runtime::fanout::fanout_location_runtime_test_access_t::lane (fanout);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    const bool blocked = lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    auto running = std::async (std::launch::async, [&] {
        runtime::fanout::fanout_location_runtime_test_access_t::run (fanout);
    });
    const bool submitted =
      wait_until ([&] { return runtime::state_lane_test_access_t::has_pending (lane); });
    runtime::fanout::fanout_location_runtime_test_access_t::request_stop (fanout);
    const bool loop_returned =
      running.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    auto pumping = std::async (std::launch::async, [&] {
        runtime::fanout::fanout_location_runtime_test_access_t::pump (fanout);
    });
    const bool posted = pumping.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    release.set_value ();
    pumping.get ();
    running.get ();
    lane.run ([] {}).get ();
    bool reentry_rejected = false;
    lane
      .run ([&] {
          try {
              (void) fanout.snapshot (channel.name);
          }
          catch (const std::logic_error &) {
              reentry_rejected = true;
          }
      })
      .get ();
    return on_lane.load () && same_sequence && coherent && blocked && submitted && loop_returned
           && posted && reentry_rejected;
}
bool verify_async_capture_releases_state_owner ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime::stateful;
    stateful_object_runtime_t objects;
    const object_ref_t object{object_kind_t::user_spot, "async-capture", 1, 1, "mesh", "node"};
    const auto created = begin_reserved_object (objects, object, "spot", {});
    if (created.error != stateful_error_t::none
        || objects.commit_create (created.attempt) != stateful_error_t::none)
        return false;
    task_completion_source_t<frozen_object_state_t> capture;
    bool entered = false;
    objects.configure_relocation_state (
      [&] (const auto &, const auto &, auto)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          entered = true;
          co_return co_await capture.task ();
      },
      [] (const auto &, const auto &, auto) -> zlink::framework::task_t<bool> { co_return true; });
    auto sealing = objects.try_seal_relocation_aggregate ({object});
    const bool suspended = entered && !sealing.await_ready ();
    const bool owner_available = objects.pending (object, turn_domain_t::application) == 0;
    capture.complete (result_t<frozen_object_state_t>::success ({object, "spot"}));
    const auto sealed = sealing.result ().value ();
    const bool passed =
      suspended && owner_available && sealed.error == stateful_error_t::none
      && objects.abort_relocation_before_cutover (sealed.seal.token).result ().value ()
           == stateful_error_t::none;
    std::cout << "async-capture suspended=" << suspended << " owner=" << owner_available
              << " passed=" << passed << '\n';
    return passed;
}
}
