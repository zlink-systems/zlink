/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/fanout/fanout_location_runtime.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/route_mesh_connection_policy.hpp"

#include <zlink/framework/contracts/configuration/module.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

/* Automatic RouteMesh discovery is a projection of the MeshNode descriptor.
 * It deliberately does not publish or consume a second peer-row model. */
class location_auto_connect_host_service_t final : public hosted_service_t,
                                                   public hosted_service_lifecycle_t
{
  public:
    location_auto_connect_host_service_t (
      message_bus_t bus,
      std::vector<channel_snapshot_t> channels,
      handler_registry_t &handlers,
      serializer_registry_t &serializers,
      std::map<std::string, std::string> client_server_advertise_hosts = {},
      std::set<std::string> route_mesh_client_channels = {},
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes = {},
      std::function<bool ()> republish_after_store_recovery = {},
      std::shared_ptr<client_server::client_server_location_runtime_t>
        client_server_runtime = nullptr) :
        _bus (std::move (bus)), _channels (std::move (channels)),
        _handlers (&handlers), _serializers (&serializers),
        _client_server_advertise_hosts (std::move (client_server_advertise_hosts)),
        _route_mesh_client_channels (std::move (route_mesh_client_channels)),
        _mesh_nodes (std::move (mesh_nodes)),
        _client_server (std::move (client_server_runtime)),
        _republish_after_store_recovery (
          std::move (republish_after_store_recovery))
    {
    }

    ~location_auto_connect_host_service_t () override { stop (); }

    bool participates_in_drain_propagation () const noexcept override
    {
        return true;
    }

    void start (service_provider_t &services) override
    {
        _runtime = &services.get_required<location_runtime_t> ();
        _store = &services.get_required<location_repository_t> ();
        _live_store = &services.get_required<live_location_reader_t> ();
        if (auto route_cache = services.get<store_location_resolvers_t> ())
            _route_cache = &route_cache->get ();

        detail::channel_runtime_manager_t manager =
          detail::channel_runtime_manager_t::from (_bus);
        manager.initialize_publisher_channels ();
        manager.initialize_client_channels ();
        manager.initialize_inbound_channels ();

        const auto needs_client_server =
          std::any_of (_channels.begin (), _channels.end (), [] (const auto &channel) {
              return (channel.server.enabled
                      && !channel.server.bind_endpoints.empty ())
                     || (channel.client.enabled
                         && (channel.client.discovery
                             || !channel.client.connect_endpoints.empty ()));
          });
        if (needs_client_server) {
            if (!_client_server)
                _client_server =
                  std::make_shared<client_server::client_server_location_runtime_t> (
                    _bus, _channels, *_runtime, *_store, *_store, services,
                    *_serializers, *_handlers, _client_server_advertise_hosts);
            _client_server->start ();
        }

        const auto needs_fanout =
          std::any_of (_channels.begin (), _channels.end (), [] (const auto &channel) {
              return (channel.publisher.enabled && channel.publisher.discovery)
                     || (channel.subscriber.enabled && channel.subscriber.discovery);
          });
        if (needs_fanout) {
            _fanout = std::make_unique<fanout::fanout_location_runtime_t> (
              _bus, _channels, *_runtime, *_store, *_store, services,
              *_serializers, *_handlers);
            _fanout->start ();
        }

        std::set<std::string> configured_meshes;
        for (const auto &route_channel_id : manager.route_channel_ids ()) {
            auto &route = manager.get_route_channel (route_channel_id);
            configured_meshes.insert (route.router_channel_id ());
            const auto manual = route.manual_connections ();
            std::optional<object_role_t> local_object_role;
            bool local_has_server_channel = false;
            for (const auto &mesh_node : _mesh_nodes) {
                if (mesh_node
                    && mesh_node->mesh_name () == route.router_channel_id ()) {
                    local_object_role = mesh_node->object_role ();
                    local_has_server_channel =
                      !mesh_node->channel_weights ().empty ();
                    break;
                }
            }
            add_loop (
              route.router_channel_id (), route.routing_id (), route.bind_endpoint (),
              local_object_role, local_has_server_channel,
              [this, &route, manual] (const target_t &target) {
                  if (std::find (manual.begin (), manual.end (), target.endpoint)
                      != manual.end ())
                      return;
                  for (const auto &mesh_node : _mesh_nodes) {
                      if (mesh_node
                          && mesh_node->mesh_name () == route.router_channel_id ()) {
                          mesh_node->expect_peer (
                            target.node_rid, target.endpoint,
                            target.lifecycle_generation,
                            target.security_identity);
                          if (target.initiates_connection)
                              mesh_node->connect_peer (
                                target.node_rid, target.endpoint,
                                target.lifecycle_generation,
                                target.security_identity);
                      }
                  }
                  if (target.initiates_connection)
                      (void) route.connect (target.node_rid, target.endpoint);
              },
              [this, &route, manual] (const target_t &target) {
                  if (std::find (manual.begin (), manual.end (), target.endpoint)
                      != manual.end ())
                      return;
                  for (const auto &mesh_node : _mesh_nodes) {
                      if (mesh_node
                          && mesh_node->mesh_name () == route.router_channel_id ()) {
                          mesh_node->forget_peer (
                            target.node_rid, target.endpoint);
                          if (target.initiates_connection)
                              mesh_node->disconnect_peer (target.endpoint);
                      }
                  }
                  if (target.initiates_connection)
                      (void) route.disconnect (target.endpoint);
              });
        }

        for (const auto &mesh_node : _mesh_nodes) {
            if (!mesh_node || configured_meshes.contains (mesh_node->mesh_name ()))
                continue;
            add_loop (
              mesh_node->mesh_name (), mesh_node->routing_id (),
              mesh_node->listen_endpoint (),
              mesh_node->object_role (),
              !mesh_node->channel_weights ().empty (),
              [mesh_node] (const target_t &target) {
                  mesh_node->expect_peer (
                    target.node_rid, target.endpoint,
                    target.lifecycle_generation,
                    target.security_identity);
                  if (target.initiates_connection)
                      mesh_node->connect_peer (
                        target.node_rid, target.endpoint,
                        target.lifecycle_generation,
                        target.security_identity);
              },
              [mesh_node] (const target_t &target) {
                  mesh_node->forget_peer (
                    target.node_rid, target.endpoint);
                  if (target.initiates_connection)
                      mesh_node->disconnect_peer (target.endpoint);
              });
        }

        _stop.store (false, std::memory_order_release);
        if (!_loops.empty ())
            detail::channel_runtime_t::from (_bus).mark_auto_connect_active ();
        for (auto &loop : _loops)
            loop.thread = std::thread ([this, &loop] { run_loop (loop); });
    }

    void stop () noexcept override
    {
        _stop.store (true, std::memory_order_release);
        if (_client_server) {
            _client_server->stop ();
            _client_server.reset ();
        }
        if (_fanout) {
            _fanout->stop ();
            _fanout.reset ();
        }
        for (auto &loop : _loops) {
            if (loop.thread.joinable ())
                loop.thread.join ();
            for (const auto &[_, target] : loop.active)
                stop_target (loop, target);
        }
        _loops.clear ();
    }

  private:
    struct target_t
    {
        std::string key;
        zlink::routing_id_t node_rid = zlink::routing_id_t::from (std::uint32_t{0});
        std::string endpoint;
        std::string owner_id;
        std::uint64_t lifecycle_generation = 0;
        std::string security_identity;
        bool initiates_connection = true;
    };

    struct loop_t
    {
        std::string mesh_name;
        std::optional<zlink::routing_id_t> local_rid;
        std::string local_endpoint;
        std::optional<object_role_t> local_object_role;
        bool local_has_server_channel = false;
        std::map<std::string, target_t> active;
        std::map<std::string, target_t> last_desired;
        std::function<void (const target_t &)> connect_target;
        std::function<void (const target_t &)> disconnect_target;
        bool recovering_from_store_failure = false;
        std::optional<std::chrono::steady_clock::time_point> failure_started_at;
        std::thread thread;
    };

    static void trace_failure (
      std::string_view stage,
      std::string_view mesh_name,
      std::string_view endpoint,
      std::string_view error) noexcept
    {
        try {
            const auto *trace = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
            if (trace == nullptr || std::string_view (trace) == "0"
                || std::string_view (trace).empty ())
                return;
            std::cerr << "zlink-cpp-auto-connect-trace stage=" << stage
                      << " mesh=" << mesh_name;
            if (!endpoint.empty ())
                std::cerr << " endpoint=" << endpoint;
            std::cerr << " error=" << error << std::endl;
        }
        catch (...) {
        }
    }

    void add_loop (std::string mesh_name,
                   std::optional<zlink::routing_id_t> local_rid,
                   std::string local_endpoint,
                   std::optional<object_role_t> local_object_role,
                   bool local_has_server_channel,
                   std::function<void (const target_t &)> connect_target,
                   std::function<void (const target_t &)> disconnect_target)
    {
        loop_t loop;
        loop.mesh_name = std::move (mesh_name);
        loop.local_rid = std::move (local_rid);
        loop.local_endpoint = std::move (local_endpoint);
        loop.local_object_role = std::move (local_object_role);
        loop.local_has_server_channel = local_has_server_channel;
        loop.connect_target = std::move (connect_target);
        loop.disconnect_target = std::move (disconnect_target);
        _loops.push_back (std::move (loop));
    }

    void run_loop (loop_t &loop)
    {
        while (!_stop.load (std::memory_order_acquire)) {
            try {
                tick (loop);
            }
            catch (const std::exception &error) {
                handle_loop_failure (loop, "tick", error.what ());
            }
            catch (...) {
                handle_loop_failure (loop, "tick", "unknown exception");
            }
            std::this_thread::sleep_for (_runtime->options ().polling_interval);
        }
    }

    void handle_loop_failure (
      loop_t &loop,
      std::string_view stage,
      std::string_view error) noexcept
    {
        trace_failure (stage, loop.mesh_name, {}, error);
        if (!loop.failure_started_at)
            loop.failure_started_at = std::chrono::steady_clock::now ();
        loop.recovering_from_store_failure = true;
        try {
            retry_pending_targets (loop);
        }
        catch (const std::exception &retry_error) {
            trace_failure (
              "recovery-failed", loop.mesh_name, {}, retry_error.what ());
        }
        catch (...) {
            trace_failure (
              "recovery-failed", loop.mesh_name, {}, "unknown exception");
        }
        try {
            _runtime->record_store_error ();
        }
        catch (...) {
        }
    }

    void stop_target (loop_t &loop, const target_t &target) noexcept
    {
        try {
            disconnect (loop, target);
        }
        catch (const std::exception &error) {
            trace_failure (
              "stop-disconnect-failed", loop.mesh_name,
              target.endpoint, error.what ());
        }
        catch (...) {
            trace_failure (
              "stop-disconnect-failed", loop.mesh_name,
              target.endpoint, "unknown exception");
        }
    }

    void tick (loop_t &loop)
    {
        std::vector<mesh_node_descriptor_t> descriptors;
        location_page_request_t page;
        do {
            auto result =
              _live_store->list_mesh_nodes (loop.mesh_name, page)
                .result ()
                .value ();
            descriptors.insert (descriptors.end (), result.items.begin (), result.items.end ());
            page.continuation_token = result.continuation_token;
        } while (page.continuation_token);

        if (loop.recovering_from_store_failure) {
            /* A successful read can precede the owner heartbeat that restores
             * the local lease. Keep existing connections until that lease and
             * every local descriptor have been published again. The next
             * polling tick then computes a diff from a complete live view. */
            if (!_runtime->owner_lease_healthy ()
                || (_republish_after_store_recovery
                    && !_republish_after_store_recovery ())) {
                retry_pending_targets (loop);
                return;
            }
            loop.recovering_from_store_failure = false;
            loop.failure_started_at.reset ();
            if (_route_cache)
                _route_cache->invalidate_all_routes_after_store_recovery ();
            return;
        }

        auto desired = compute_desired (loop, descriptors);
        loop.last_desired = desired;
        _runtime->observe_discovered_peers (desired.size ());
        for (auto it = loop.active.begin (); it != loop.active.end ();) {
            if (!desired.contains (it->first)) {
                disconnect (loop, it->second);
                it = loop.active.erase (it);
            } else {
                ++it;
            }
        }
        for (const auto &[key, target] : desired) {
            const auto current = loop.active.find (key);
            if (current == loop.active.end ()) {
                connect (loop, target);
                loop.active[key] = target;
            } else if (current->second.endpoint != target.endpoint
                       || current->second.owner_id != target.owner_id
                       || current->second.lifecycle_generation
                            != target.lifecycle_generation) {
                disconnect (loop, current->second);
                connect (loop, target);
                loop.active[key] = target;
            }
        }
    }

    static std::map<std::string, target_t>
    compute_desired (const loop_t &loop,
                     const std::vector<mesh_node_descriptor_t> &descriptors)
    {
        std::map<std::string, target_t> desired;
        const auto local = loop.local_rid
          ? std::find_if (
              descriptors.begin (), descriptors.end (),
              [&loop] (const mesh_node_descriptor_t &descriptor) {
                  return descriptor.mesh_name == loop.mesh_name
                         && descriptor.rid.to_hex ()
                              == loop.local_rid->to_hex ();
              })
          : descriptors.end ();
        for (const auto &descriptor : descriptors) {
            if (descriptor.mesh_name != loop.mesh_name || descriptor.endpoint.empty ()
                || (loop.local_rid
                    && descriptor.rid.to_hex () == loop.local_rid->to_hex ())
                || descriptor.state == framework_runtime_state_t::relocating
                || descriptor.state == framework_runtime_state_t::relocated
                || descriptor.state == framework_runtime_state_t::draining
                || descriptor.state == framework_runtime_state_t::stopped
                || descriptor.state == framework_runtime_state_t::error)
                continue;
            const auto remote_has_server_channel =
              !descriptor.channel_weights.empty ();
            if (loop.local_object_role
                && mesh::route_mesh_connection_not_required (
                  *loop.local_object_role, loop.local_has_server_channel,
                  descriptor.object_role, remote_has_server_channel))
                continue;
            if (!loop.local_object_role
                && local != descriptors.end ()
                && mesh::route_mesh_connection_not_required (
                  *local, descriptor))
                continue;
            /* Both sides retain the discovery expectation so inbound
             * admission can validate endpoint and security. Only the lower
             * RID starts the physical connection. */
            const auto initiates_connection =
              loop.local_endpoint.empty () || !loop.local_rid
              || loop.local_rid->to_hex () < descriptor.rid.to_hex ();
            const auto key = descriptor.rid.to_hex ();
            desired.emplace (
              key, target_t{key, descriptor.rid, descriptor.endpoint,
                            descriptor.owner_id, descriptor.lifecycle_generation,
                            descriptor.security_identity, initiates_connection});
        }
        return desired;
    }

    void retry_pending_targets (loop_t &loop)
    {
        if (!loop.failure_started_at
            || std::chrono::steady_clock::now () - *loop.failure_started_at
                 > _runtime->options ().store_failure_grace)
            return;
        for (const auto &[key, target] : loop.last_desired) {
            if (!loop.active.contains (key)) {
                connect (loop, target);
                loop.active[key] = target;
            }
        }
    }

    static void connect (loop_t &loop, const target_t &target)
    {
        if (loop.connect_target)
            loop.connect_target (target);
    }

    static void disconnect (loop_t &loop, const target_t &target)
    {
        if (loop.disconnect_target)
            loop.disconnect_target (target);
    }

    message_bus_t _bus;
    std::vector<channel_snapshot_t> _channels;
    handler_registry_t *_handlers;
    serializer_registry_t *_serializers;
    std::map<std::string, std::string> _client_server_advertise_hosts;
    std::set<std::string> _route_mesh_client_channels;
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> _mesh_nodes;
    location_runtime_t *_runtime = nullptr;
    location_repository_t *_store = nullptr;
    live_location_reader_t *_live_store = nullptr;
    store_location_resolvers_t *_route_cache = nullptr;
    std::atomic_bool _stop{false};
    std::vector<loop_t> _loops;
    std::shared_ptr<client_server::client_server_location_runtime_t> _client_server;
    std::unique_ptr<fanout::fanout_location_runtime_t> _fanout;
    std::function<bool ()> _republish_after_store_recovery;
};

} // namespace zlink::framework::runtime
