/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/location_reclaim_clock.hpp"

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
#include "runtime/locations/store_location_resolvers.hpp"
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

namespace zlink::framework::test
{
void close_test_completion_owner ();
}

namespace
{
using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

void destroy_location_with_busy_lane (auto prepare)
{
    using zlink::framework::runtime::location_runtime_test_access_t;
    in_memory_location_repository_t store;
    auto runtime = std::make_unique<location_runtime_t> (store);
    prepare (*runtime, store);
    if (::testing::Test::HasFailure ())
        return;
    zlink::framework::test::close_test_completion_owner ();
    ASSERT_TRUE (zlink::framework::detail::observe_completion_dispatcher ().expired ());

    auto &lane = location_runtime_test_access_t::lane (*runtime);
    std::latch entered (1);
    ASSERT_TRUE (lane.try_post ([&] {
        entered.count_down ();
        while (!lane.closed ())
            std::this_thread::yield ();
    }));
    entered.wait ();
    runtime.reset ();
}

TEST (LocationRuntimeDeathTest, DestroysStoppedRuntimeWithoutCompletionOwner)
{
    ASSERT_EXIT (
      {
          destroy_location_with_busy_lane ([] (auto &runtime, auto &) {
              runtime.start (zlink::routing_id_t::from ("location-stop-once"));
              runtime.stop ();
          });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (LocationRuntimeDeathTest, DestroysUnstartedRuntimeWithoutCompletionOwner)
{
    ASSERT_EXIT (
      {
          destroy_location_with_busy_lane ([] (auto &, auto &) {});
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (LocationRuntimeDeathTest, DestroysUnstartedRuntimeAfterPublicationWithoutCompletionOwner)
{
    ASSERT_EXIT (
      {
          destroy_location_with_busy_lane ([] (auto &runtime, auto &store) {
              using namespace zlink::framework;
              const auto claim =
                store.claim_owner_lease ("publication-owner", std::chrono::minutes (1))
                  .result ()
                  .value ();
              const auto &token = std::get<owner_lease_claimed_t> (claim).token;
              mesh_node_descriptor_t descriptor;
              descriptor.mesh_name = "publication-mesh";
              descriptor.rid = zlink::routing_id_t::from ("publication-node");
              descriptor.endpoint = "inproc://publication-node";
              descriptor.owner_id = token.owner_id;
              descriptor.lease_generation = token.lease_generation;
              descriptor.lifecycle_generation = 1;
              descriptor.descriptor_revision = 1;
              descriptor.security_identity = "default";
              descriptor.activation_concurrency.limit = 1;
              ASSERT_TRUE (runtime
                             .descriptor_write_confirmed ([&] {
                                 return store.update_mesh_node (descriptor,
                                                                location_write_intent_t::new_claim);
                             })
                             .result ()
                             .value ());
          });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
} // namespace
