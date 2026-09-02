/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/messages.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace e2e = zlink::framework::e2e::instance_spot;
namespace fw = zlink::framework;

namespace
{

struct role_options_t
{
    std::string role;
    std::string rid;
    std::string http_endpoint;
    std::string mesh_endpoint;
    std::string peer_rid;
    std::string peer_endpoint;
    std::string second_peer_rid;
    std::string second_peer_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::string evidence_file;

    static role_options_t bind (const fw::configuration_section_t &section)
    {
        return {.role = section.require ("role"),
                .rid = section.require ("rid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .mesh_endpoint = section.require ("meshEndpoint"),
                .peer_rid = section.get ("peerRid").value_or (""),
                .peer_endpoint = section.get ("peerEndpoint").value_or (""),
                .second_peer_rid = section.get ("secondPeerRid").value_or (""),
                .second_peer_endpoint = section.get ("secondPeerEndpoint").value_or (""),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir"),
                .evidence_file = section.get ("evidenceFile").value_or ("")};
    }
};

class shutdown_state_t
{
  public:
    void request () noexcept { _requested.store (true, std::memory_order_release); }

    bool requested () const noexcept
    {
        return _requested.load (std::memory_order_acquire);
    }

  private:
    std::atomic<bool> _requested {false};
};

class evidence_store_t
{
  public:
    evidence_store_t (std::string owner_rid, std::string evidence_file) :
        _owner_rid (std::move (owner_rid)), _evidence_file (std::move (evidence_file))
    {
    }

    void materialized ()
    {
        std::lock_guard lock (_mutex);
        ++_lifecycle.materialized_instances;
        persist_lifecycle_locked ();
    }

    void closed ()
    {
        std::lock_guard lock (_mutex);
        ++_lifecycle.closed_instances;
        persist_lifecycle_locked ();
    }

    void entered (const std::string &operation_id, const std::string &spot_id)
    {
        std::lock_guard lock (_mutex);
        auto &entry = _operations[operation_id];
        ++entry.entered;
        entry.spot_id = spot_id;
        entry.owner_rid = _owner_rid;
        entry.instance_count = _lifecycle.materialized_instances;
        persist_operation_locked (operation_id, entry);
    }

    void completed (const std::string &operation_id, const std::string &spot_id)
    {
        std::lock_guard lock (_mutex);
        auto &entry = _operations[operation_id];
        ++entry.completed;
        entry.spot_id = spot_id;
        entry.owner_rid = _owner_rid;
        entry.instance_count = _lifecycle.materialized_instances;
        persist_operation_locked (operation_id, entry);
    }

    e2e::operation_evidence_t operation (const std::string &operation_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _operations.find (operation_id);
        if (found == _operations.end ()) {
            return {.instance_count = _lifecycle.materialized_instances,
                    .owner_rid = _owner_rid};
        }
        return found->second;
    }

    e2e::lifecycle_snapshot_t lifecycle () const
    {
        std::lock_guard lock (_mutex);
        return _lifecycle;
    }

  private:
    void persist_operation_locked (const std::string &operation_id,
                                   const e2e::operation_evidence_t &entry) const
    {
        if (_evidence_file.empty ()) {
            return;
        }
        std::ofstream file (_evidence_file, std::ios::app);
        if (file) {
            file << nlohmann::json{{"operationId", operation_id}, {"evidence", entry}}
                        .dump ()
                 << '\n';
        }
    }

    void persist_lifecycle_locked () const
    {
        if (_evidence_file.empty ()) {
            return;
        }
        std::ofstream file (_evidence_file, std::ios::app);
        if (file) {
            file << nlohmann::json{{"lifecycle", _lifecycle}}.dump () << '\n';
        }
    }

    std::string _owner_rid;
    std::string _evidence_file;
    mutable std::mutex _mutex;
    std::map<std::string, e2e::operation_evidence_t> _operations;
    e2e::lifecycle_snapshot_t _lifecycle;
};

class scenario_instance_spot_t final : public fw::instance_spot_t
{
  public:
    scenario_instance_spot_t (fw::instance_spot_context_t context,
                              evidence_store_t &evidence) :
        _context (std::move (context)), _evidence (evidence)
    {
    }

    fw::instance_spot_context_t &context () noexcept override { return _context; }

    const fw::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ()
          .add_handler<&scenario_instance_spot_t::handle_request> (
            e2e::probe_req_t::packet_name)
          .add_handler<&scenario_instance_spot_t::handle_send> (
            e2e::probe_msg_t::packet_name);
    }

    fw::task_t<void> on_initialize () override
    {
        _evidence.materialized ();
        co_return;
    }

    fw::task_t<void> on_closing (const fw::spot_closing_context_t &, std::stop_token) override
    {
        _evidence.closed ();
        co_return;
    }

    e2e::probe_res_t handle_request (const e2e::probe_req_t &request)
    {
        _evidence.entered (request.operation_id, request.spot_id);
        _evidence.completed (request.operation_id, request.spot_id);
        return {.spot_id = request.spot_id,
                .operation_id = request.operation_id,
                .action = request.action,
                .instance_spot = true};
    }

    void handle_send (const e2e::probe_msg_t &message)
    {
        _evidence.entered (message.operation_id, message.spot_id);
        _evidence.completed (message.operation_id, message.spot_id);
    }

  private:
    fw::instance_spot_context_t _context;
    evidence_store_t &_evidence;
};

fw::http_response_t json_response (nlohmann::json value, int status = 200)
{
    return {.status = status, .body = std::move (value).dump ()};
}

class ready_handler_t
{
  public:

    explicit ready_handler_t (fw::route_mesh_runtime_t &runtime) : _runtime (runtime) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto target = request.query_values.find ("targetRid");
        if (target == request.query_values.end () || target->second.empty ()) {
            return json_response ({ {"error", "targetRid is required"} }, 400);
        }
        const auto snapshot = _runtime.snapshot (e2e::mesh_name);
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == target->second
                && peer.state == fw::peer_state_t::ready) {
                return json_response ({ {"ready", true}, {"targetRid", target->second} });
            }
        }
        return json_response ({ {"ready", false}, {"targetRid", target->second} }, 503);
    }

  private:
    fw::route_mesh_runtime_t &_runtime;
};

class evidence_handler_t
{
  public:

    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto operation = request.query_values.find ("operationId");
        if (operation == request.query_values.end () || operation->second.empty ()) {
            return json_response ({ {"error", "operationId is required"} }, 400);
        }
        return json_response (_evidence.operation (operation->second));
    }

  private:
    evidence_store_t &_evidence;
};

class lifecycle_handler_t
{
  public:

    explicit lifecycle_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    fw::http_response_t handle (const fw::http_request_t &)
    {
        return json_response (_evidence.lifecycle ());
    }

  private:
    evidence_store_t &_evidence;
};

class shutdown_handler_t
{
  public:

    explicit shutdown_handler_t (shutdown_state_t &state) : _state (state) {}

    fw::http_response_t handle (const fw::http_request_t &)
    {
        _state.request ();
        return json_response ({ {"stopping", true} });
    }

  private:
    shutdown_state_t &_state;
};

class instance_request_handler_t
{
  public:

    explicit instance_request_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::probe_req_t> ();
        try {
            const auto reply = co_await _routes
                                 .request_to_spot (request.spot_id, request)
                                 .instance_spot (e2e::spot_type)
                                 .in_mesh (e2e::mesh_name)
                                 .timeout (std::chrono::seconds (5))
                                 .async<e2e::probe_res_t> ();
            co_return json_response (reply);
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response ({ {"error", error.what ()},
                                       {"errorKind", static_cast<int> (error.kind ())} },
                                      500);
        }
    }

  private:
    fw::route_client_t &_routes;
};

class instance_send_handler_t
{
  public:

    explicit instance_send_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::probe_msg_t> ();
        try {
            co_await _routes
              .send_to_spot (request.spot_id, request)
              .instance_spot (e2e::spot_type)
              .in_mesh (e2e::mesh_name)
              .async ();
            co_return json_response ({ {"status", "accepted"},
                                       {"spotId", request.spot_id},
                                       {"operationId", request.operation_id},
                                       {"action", request.action} });
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response ({ {"error", error.what ()},
                                       {"errorKind", static_cast<int> (error.kind ())} },
                                      500);
        }
    }

  private:
    fw::route_client_t &_routes;
};

} // namespace

int main (int argc, char **argv)
{
    try {
        auto app = fw::app_t::create ();
        app.config ().load_cli (argc, argv);
        const auto config_path = app.config ().model ().get ("config");
        if (!config_path) {
            throw std::runtime_error ("InstanceSpot role requires --config=<path>");
        }
        app.config ().load_json (*config_path);
        const auto options = app.config ().bind_required<role_options_t> ("e2e");
        if (options.role != "owner" && options.role != "caller") {
            throw std::runtime_error ("InstanceSpot role must be owner or caller");
        }
        if (options.role == "caller"
            && (options.peer_rid.empty () || options.peer_endpoint.empty ())) {
            throw std::runtime_error ("InstanceSpot caller requires peerRid and peerEndpoint");
        }
        std::filesystem::create_directories (options.log_dir);
        app.logging ().use_file (options.log_dir + "/" + options.rid + ".log")
          .set_min_level (fw::log_level_t::debug);

        std::atomic<shutdown_state_t *> shutdown_pointer {nullptr};
        app.add_zlink_framework ([options, &shutdown_pointer] (
                                   fw::zlink_framework_options_t &framework) {
            auto evidence = std::make_unique<evidence_store_t> (
              options.rid, options.evidence_file);
            auto *evidence_ptr = evidence.get ();
            framework.services ().add_singleton<evidence_store_t> (std::move (evidence));
            auto shutdown = std::make_unique<shutdown_state_t> ();
            auto *shutdown_ptr = shutdown.get ();
            shutdown_pointer.store (shutdown_ptr, std::memory_order_release);
            framework.services ().add_singleton<shutdown_state_t> (std::move (shutdown));

            framework.add_location_store (
              std::make_shared<fw::redis::redis_location_store_t> (
                fw::redis::redis_location_options_t{
                  .connection_string = options.redis_endpoint,
                  .key_prefix = options.redis_key_prefix}));
            framework.add_relocation_store (
              std::make_shared<fw::redis::redis_relocation_store_t> (
                fw::redis::redis_relocation_options_t{
                  .connection_string = options.redis_endpoint,
                  .key_prefix = options.redis_key_prefix + ":relocation"}));
            auto &locations = framework.configure_locations ();
            locations.polling_interval = std::chrono::milliseconds (100);
            locations.owner_lease_renew_interval = std::chrono::milliseconds (100);
            locations.owner_lease_ttl = std::chrono::seconds (5);
            locations.owner_lease_fencing_margin = std::chrono::milliseconds (500);
            locations.owner_lease_renew_timeout = std::chrono::milliseconds (500);

            auto mesh = framework.add_route_mesh (e2e::mesh_name)
                          .listen (options.mesh_endpoint)
                          .set_routing_id (zlink::routing_id_t::from (options.rid));
            if (options.role == "owner") {
                mesh.set_object_role (fw::object_role_t::server);
                mesh.channel_name (e2e::mesh_name).server ();
                mesh.add_instance_spot_factory<scenario_instance_spot_t> (
                    e2e::spot_type,
                    [evidence_ptr] (fw::instance_spot_context_t context) {
                        return std::make_shared<scenario_instance_spot_t> (
                          std::move (context), *evidence_ptr);
                    },
                    [] (auto &factory) { factory.disable_relocation (); });
            } else {
                mesh.set_object_role (fw::object_role_t::client);
                mesh.channel_name (e2e::mesh_name).client ();
                mesh.peer_connections ().connect (
                  zlink::routing_id_t::from (options.peer_rid), options.peer_endpoint);
                if (!options.second_peer_rid.empty ()
                    && !options.second_peer_endpoint.empty ()) {
                    mesh.peer_connections ().connect (
                      zlink::routing_id_t::from (options.second_peer_rid),
                      options.second_peer_endpoint);
                }
            }

            framework.http ()
              .listen (options.http_endpoint)
              .map_health ("/health")
              .map_get<ready_handler_t> ("/ready")
              .map_get<evidence_handler_t> ("/evidence")
              .map_get<lifecycle_handler_t> ("/lifecycle")
              .map_post<shutdown_handler_t> ("/shutdown");
            if (options.role == "caller") {
                framework.http ()
                  .map_post<instance_request_handler_t> ("/instance/request")
                  .map_post<instance_send_handler_t> ("/instance/send");
            }
        });

        std::thread shutdown_watcher ([&app, &shutdown_pointer] {
            shutdown_state_t *state = nullptr;
            while ((state = shutdown_pointer.load (std::memory_order_acquire)) == nullptr) {
                std::this_thread::sleep_for (std::chrono::milliseconds (20));
            }
            while (!state->requested ()) {
                std::this_thread::sleep_for (std::chrono::milliseconds (20));
            }
            app.request_stop ();
        });
        int code = 0;
        try {
            code = app.run (argc, argv);
        }
        catch (...) {
            if (auto *state = shutdown_pointer.load (std::memory_order_acquire)) {
                state->request ();
            }
            if (shutdown_watcher.joinable ()) {
                shutdown_watcher.join ();
            }
            throw;
        }
        if (auto *state = shutdown_pointer.load (std::memory_order_acquire)) {
            state->request ();
        }
        shutdown_watcher.join ();
        return code;
    }
    catch (const std::exception &error) {
        std::cerr << "instance-spot role failed: " << error.what () << "\n";
        return 2;
    }
}
