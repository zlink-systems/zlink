/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime_failure_fixture.hpp"
#include "location_runtime_test_access.hpp"
#include "location_reclaim_clock.hpp"

#include "runtime_metric_contract.hpp"
#include "manual_deadlines.hpp"
#include "allocation_counter.hpp"
#include "manual_internal_scheduler.hpp"

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

namespace
{
using zlink::framework::location_options_t;

constexpr auto heartbeat_observation_timeout = std::chrono::seconds (5);
constexpr auto admission_observation_timeout = std::chrono::milliseconds (100);
const location_options_t asynchronous_heartbeat_options{
  .owner_lease_renew_interval = std::chrono::milliseconds (5),
  .owner_lease_ttl = std::chrono::seconds (15)};
} // namespace
