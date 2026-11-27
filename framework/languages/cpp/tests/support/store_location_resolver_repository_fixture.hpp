/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/json.hpp>
#include "runtime_failure_fixture.hpp"
#include "host_readiness.hpp"
#include "location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::test::store_location_resolvers
{
using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_object_entry_t;
using zlink::framework::location_object_filter_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_page_t;
using zlink::framework::location_role_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_resolvers_t;
using zlink::framework::runtime::store_location_runtime_query_t;

location_owner_token_t live_owner_token (zlink::framework::location_repository_t &store,
                                         const std::string &owner_id);

std::vector<std::byte> user_spot_authority_payload (std::string_view spot_id,
                                                    std::uint64_t generation,
                                                    std::string_view owner_id = "owner-a",
                                                    std::uint64_t owner_lease_generation = 1);

void seed_mesh_node (in_memory_location_repository_t &store,
                     std::string owner_id,
                     std::string mesh_name,
                     std::string rid,
                     std::string endpoint,
                     zlink::framework::framework_runtime_state_t state =
                       zlink::framework::framework_runtime_state_t::serving);

class test_location_repository_t : public zlink::framework::location_repository_t
{
  public:
    test_location_repository_t ()
    {
        _inner.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    }

    bool fail_authority_queries = false;
    std::function<void ()> before_owner_read;

    void set_authority (std::string key, zlink::framework::authority_snapshot_t snapshot)
    {
        authorities.insert_or_assign (std::move (key), std::move (snapshot));
    }

    void set_authority (const std::string &key, std::nullopt_t) { authorities.erase (key); }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t descriptor,
                      zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_mesh_node (std::move (descriptor), intent);
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_mesh_node (zlink::framework::mesh_node_descriptor_key_t key,
                      zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_mesh_node (std::move (key), std::move (owner));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh_name,
                          zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_mesh_nodes (std::move (mesh_name), std::move (page));
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_client_server (zlink::framework::client_server_server_descriptor_t descriptor,
                          zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_client_server (std::move (descriptor), intent);
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_client_server (zlink::framework::client_server_server_descriptor_key_t key,
                          zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_client_server (std::move (key), std::move (owner));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name,
                         zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_client_servers (std::move (channel_name), std::move (page));
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_fanout_publisher (zlink::framework::fanout_publisher_descriptor_t descriptor,
                             zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_fanout_publisher (std::move (descriptor), intent);
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_fanout_publisher (zlink::framework::fanout_publisher_descriptor_key_t key,
                             zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_fanout_publisher (std::move (key), std::move (owner));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::fanout_publisher_descriptor_t>>
    list_fanout_publishers (std::string channel_name,
                            zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_fanout_publishers (std::move (channel_name), std::move (page));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override
    {
        return _inner.claim_owner_lease (std::move (owner_id), lease_ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        ++owner_read_count;
        if (auto hook = std::exchange (before_owner_read, {}))
            hook ();
        return _inner.read_owner_lease (std::move (owner_id));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t token,
                       std::chrono::milliseconds lease_ttl) override
    {
        return _inner.renew_owner_lease (std::move (token), lease_ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_release_result_t>
    release_owner_lease (zlink::framework::location_owner_token_t token) override
    {
        return _inner.release_owner_lease (std::move (token));
    }

    zlink::framework::task_t<std::int64_t>
    remove_all_by_owner (zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_all_by_owner (std::move (owner));
    }

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t key,
                         std::stop_token cancellation = {}) override
    {
        ++authority_read_count;
        if (fail_authority_queries) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "injected authority read failure"));
        }
        if (key.value.starts_with ("zla1:a:"))
            resolve_actor_count.fetch_add (1, std::memory_order_relaxed);
        if (const auto found = authorities.find (key.value); found != authorities.end ()) {
            auto snapshot = found->second;
            snapshot.store_now = std::chrono::system_clock::now ();
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{std::move (snapshot)}));
        }
        return _inner.read_authority (std::move (key), cancellation);
    }

    zlink::framework::task_t<zlink::framework::authority_compare_exchange_result_t>
    compare_exchange_authority_core (zlink::framework::authority_key_t key,
                                     std::string expected_store_version,
                                     zlink::framework::authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override
    {
        return _inner.compare_exchange_authority (
          std::move (key), std::move (expected_store_version), std::move (mutation), cancellation);
    }

    zlink::framework::task_t<zlink::framework::authority_scan_result_t>
    list_authorities (std::string prefix,
                      std::optional<zlink::framework::authority_scan_cursor_t> cursor,
                      std::size_t limit,
                      std::stop_token cancellation = {}) override
    {
        if (fail_authority_queries) {
            return zlink::framework::task_t<zlink::framework::authority_scan_result_t> (
              zlink::framework::result_t<zlink::framework::authority_scan_result_t>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "injected authority scan failure"));
        }
        return _inner.list_authorities (std::move (prefix), std::move (cursor), limit,
                                        cancellation);
    }

    zlink::framework::task_t<std::optional<zlink::framework::creation_terminal_record_t>>
    read_creation_terminal (zlink::framework::creation_operation_identity_t operation,
                            std::stop_token cancellation = {}) override
    {
        return _inner.read_creation_terminal (std::move (operation), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_reserve_result_t>
    reserve_core (zlink::framework::object_reserve_request_t request,
                  std::stop_token cancellation = {}) override
    {
        if (force_reserve_conflict.exchange (false, std::memory_order_acq_rel)) {
            return zlink::framework::task_t<zlink::framework::object_reserve_result_t> (
              zlink::framework::result_t<zlink::framework::object_reserve_result_t>::success (
                zlink::framework::object_reserve_conflict_t{
                  zlink::framework::authority_snapshot_t{}}));
        }
        return _inner.reserve (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_complete_creation_result_t>
    complete_creation_core (zlink::framework::object_complete_creation_request_t request,
                            std::stop_token cancellation = {}) override
    {
        return _inner.complete_creation (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_commit_result_t>
    commit_core (zlink::framework::object_commit_request_t request,
                 std::stop_token cancellation = {},
                 std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        return _inner.commit (std::move (request), cancellation, operation_deadline);
    }

    zlink::framework::task_t<zlink::framework::object_abort_result_t>
    abort (zlink::framework::object_abort_request_t request,
           std::stop_token cancellation = {},
           std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        abort_count.fetch_add (1, std::memory_order_relaxed);
        return _inner.abort (std::move (request), cancellation, operation_deadline);
    }

    zlink::framework::task_t<zlink::framework::aggregate_prepare_result_t>
    prepare_aggregate_core (zlink::framework::aggregate_prepare_request_t request,
                            std::stop_token cancellation = {}) override
    {
        return _inner.prepare_aggregate (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::aggregate_commit_result_t>
    commit_aggregate (zlink::framework::aggregate_fence_t fence,
                      std::stop_token cancellation = {}) override
    {
        return _inner.commit_aggregate (std::move (fence), cancellation);
    }

    zlink::framework::task_t<zlink::framework::aggregate_abort_result_t>
    abort_aggregate (zlink::framework::aggregate_fence_t fence,
                     std::stop_token cancellation = {}) override
    {
        return _inner.abort_aggregate (std::move (fence), cancellation);
    }

    std::atomic_size_t abort_count{0};
    std::atomic_size_t resolve_spot_count{0};
    std::atomic_size_t resolve_actor_count{0};
    std::atomic_size_t authority_read_count{0};
    std::atomic_size_t owner_read_count{0};
    std::atomic_bool force_reserve_conflict{false};

  private:
    std::map<std::string, zlink::framework::authority_snapshot_t> authorities;
    in_memory_location_repository_t _inner;
};

template <typename Store>
location_owner_token_t claim_test_owner (Store &store,
                                         std::string owner_id,
                                         std::chrono::milliseconds ttl = std::chrono::seconds (30))
{
    const auto claimed = store.claim_owner_lease (owner_id, ttl).result ().value ();
    const auto *value = std::get_if<zlink::framework::owner_lease_claimed_t> (&claimed);
    if (value == nullptr)
        throw std::runtime_error ("test owner lease claim failed");
    return value->token;
}
} // namespace zlink::framework::test::store_location_resolvers
