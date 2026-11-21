/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <zlink/framework/contracts/locations/runtime_query.hpp>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace framework_mesh_node_vertical_fixture
{
class monitoring_mesh_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    void set_local (zlink::framework::mesh_node_descriptor_t descriptor)
    {
        std::lock_guard lock (_mutex);
        _descriptor = std::move (descriptor);
    }

    zlink::framework::mesh_node_descriptor_t local () const
    {
        std::lock_guard lock (_mutex);
        return _descriptor;
    }

    void set_remote (std::optional<zlink::framework::mesh_node_descriptor_t> descriptor)
    {
        std::lock_guard lock (_mutex);
        _remote = std::move (descriptor);
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t value,
                      zlink::framework::location_write_intent_t) override
    {
        std::lock_guard lock (_mutex);
        _descriptor = std::move (value);
        return zlink::framework::task_t<zlink::framework::location_write_result_t> (
          zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
            zlink::framework::location_write_result_t::stored (1, {})));
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_mesh_node (zlink::framework::mesh_node_descriptor_key_t,
                      zlink::framework::location_owner_token_t) override
    {
        return zlink::framework::task_t<zlink::framework::location_write_status_t> (
          zlink::framework::result_t<zlink::framework::location_write_status_t>::success (
            zlink::framework::location_write_status_t::stored));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh_name,
                          zlink::framework::location_page_request_t = {}) override
    {
        std::lock_guard lock (_mutex);
        zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t> page;
        if (_descriptor.mesh_name == mesh_name)
            page.items.push_back (_descriptor);
        if (_remote && _remote->mesh_name == mesh_name)
            page.items.push_back (*_remote);
        return zlink::framework::task_t<decltype (page)> (
          zlink::framework::result_t<decltype (page)>::success (std::move (page)));
    }

  private:
    mutable std::mutex _mutex;
    zlink::framework::mesh_node_descriptor_t _descriptor;
    std::optional<zlink::framework::mesh_node_descriptor_t> _remote;
};

class monitoring_location_query_t final : public zlink::framework::location_runtime_query_t
{
  public:
    void set_store_healthy (bool healthy) { _store_healthy.store (healthy); }
    std::function<void ()> on_unhealthy_status;

    zlink::framework::task_t<zlink::framework::location_runtime_status_t> get_status () override
    {
        zlink::framework::location_runtime_status_t status;
        status.store_healthy = _store_healthy.load ();
        if (!status.store_healthy && on_unhealthy_status) {
            auto callback = std::move (on_unhealthy_status);
            callback ();
        }
        co_return status;
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::location_topology_entry_t>>
    list_topology (zlink::framework::location_topology_filter_t,
                   zlink::framework::location_page_request_t = {}) override
    {
        co_return zlink::framework::location_page_t<zlink::framework::location_topology_entry_t>{};
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::location_service_summary_t>>
    list_service_summaries (zlink::framework::location_service_summary_filter_t,
                            zlink::framework::location_page_request_t = {}) override
    {
        co_return zlink::framework::location_page_t<zlink::framework::location_service_summary_t>{};
    }

    zlink::framework::task_t<std::optional<zlink::framework::location_object_entry_t>>
    find_actor_location (zlink::framework::actor_id_t) override
    {
        co_return std::nullopt;
    }

    zlink::framework::task_t<std::optional<zlink::framework::location_object_entry_t>>
    find_spot_location (zlink::framework::spot_id_t) override
    {
        co_return std::nullopt;
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::location_object_entry_t>>
    list_object_locations (zlink::framework::location_object_filter_t,
                           zlink::framework::location_page_request_t = {}) override
    {
        co_return zlink::framework::location_page_t<zlink::framework::location_object_entry_t>{};
    }

  private:
    std::atomic_bool _store_healthy{true};
};

// Holds the first Store result after it has captured the old descriptor. The
// monitor starts on another thread because its initial Store refresh is synchronous.
class delayed_monitoring_mesh_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    explicit delayed_monitoring_mesh_store_t (zlink::framework::mesh_node_descriptor_t descriptor) :
        _descriptor (std::move (descriptor)), _release (_released.get_future ().share ())
    {
    }

    std::future<void> first_read () { return _read.get_future (); }
    void release () { _released.set_value (); }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string, zlink::framework::location_page_request_t = {}) override
    {
        zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t> page;
        page.items.push_back (_descriptor);
        std::call_once (_first_read, [&] {
            _read.set_value ();
            _release.wait ();
        });
        co_return page;
    }

  private:
    zlink::framework::mesh_node_descriptor_t _descriptor;
    std::promise<void> _read;
    std::promise<void> _released;
    std::shared_future<void> _release;
    std::once_flag _first_read;
};
} // namespace framework_mesh_node_vertical_fixture
