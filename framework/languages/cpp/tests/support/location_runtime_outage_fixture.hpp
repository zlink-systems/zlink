/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "location_runtime_wait.hpp"
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

namespace zlink::framework::runtime
{
using namespace std::chrono_literals;
using lease_access = location_runtime_test_access_t;
using monotonic_time = std::chrono::steady_clock::time_point;

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
} // namespace zlink::framework::runtime
