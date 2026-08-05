/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../Shared/configuration.hpp"
#include "../../Shared/messages.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace e2e = zlink::e2e::to_actor_messaging;
namespace fw = zlink::framework;

namespace
{

void add_redis_location_store (fw::zlink_framework_options_t &framework,
                               const e2e::redis_configuration_t &redis)
{
    framework.add_location_store (
      std::make_shared<fw::redis::redis_location_store_t> (
        fw::redis::redis_location_options_t{
          .connection_string = redis.endpoint, .key_prefix = redis.key_prefix}));
    auto &locations = framework.configure_locations ();
    locations.heartbeat_interval = std::chrono::seconds (1);
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.polling_interval = std::chrono::milliseconds (500);
}

class session_evidence_store_t
{
  public:
    void append (e2e::actor_evidence_t entry)
    {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _entries.push_back (std::move (entry));
        }
        _changed.notify_all ();
    }

    std::vector<e2e::actor_evidence_t> all () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _entries;
    }

    bool wait (const std::string &actor_id, const std::string &kind)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        const auto found = [&] {
            for (const auto &entry : _entries) {
                if (entry.actor_id == actor_id && entry.kind == kind) {
                    return true;
                }
            }
            return false;
        };
        return _changed.wait_for (lock, std::chrono::seconds (5), found);
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<e2e::actor_evidence_t> _entries;
};

class actor_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::session_actor_manager_t, fw::actor_directory_t,
                            session_evidence_store_t, e2e::session_configuration_t>;

    actor_session_t (fw::session_actor_manager_t &actors,
                     fw::actor_directory_t &directory,
                     session_evidence_store_t &evidence,
                     e2e::session_configuration_t &configuration) :
        _actors (actors), _directory (directory), _evidence (evidence),
        _gateway_rid (configuration.node_rid)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }

    fw::task_t<void> on_disconnected (fw::stream_t &) override
    {
        if (!_bound_actor_id.empty ()) {
            _evidence.append ({_bind_scenario, _bound_actor_id, "disconnect", _gateway_rid});
        }
        co_return;
    }

    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        if (dispatch.packet_name != e2e::bind_actor_session_req_t::packet_name) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                             "actor session expects BindActorSessionReq");
        }
        const auto request = payload.parse_json<e2e::bind_actor_session_req_t> ();
        const auto actor_ref = co_await _directory.find (request.actor_id);
        if (!actor_ref) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "actor session target was not found");
        }
        auto bound = co_await _actors.bind_or_get (*actor_ref).submit ();
        _bound_actor_id = std::string (bound.actor_id ());
        _bind_scenario = request.scenario;
        _evidence.append ({request.scenario, _bound_actor_id, "bind", _gateway_rid});
        stream
          .reply_packet (zlink::message_t::from_json (
            e2e::bind_actor_session_res_t{request.scenario, _bound_actor_id}))
          .submit ();
    }

  private:
    fw::session_actor_manager_t &_actors;
    fw::actor_directory_t &_directory;
    session_evidence_store_t &_evidence;
    std::string _bound_actor_id;
    std::string _bind_scenario;
    std::string _gateway_rid;
};

class evidence_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<session_evidence_store_t>;
    explicit evidence_handler_t (session_evidence_store_t &evidence) : _evidence (evidence) {}

    fw::http_response_t handle (const fw::http_request_t &)
    {
        fw::http_response_t response;
        response.body = nlohmann::json (_evidence.all ()).dump ();
        return response;
    }

  private:
    session_evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<session_evidence_store_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;
    explicit evidence_wait_handler_t (session_evidence_store_t &evidence) : _evidence (evidence) {}

    e2e::actor_call_response_t handle (const e2e::actor_call_request_t &request)
    {
        return {request.scenario, request.actor_id,
                _evidence.wait (request.actor_id, request.value) ? "observed" : "timeout", ""};
    }

  private:
    session_evidence_store_t &_evidence;
};

} // namespace

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    const auto configuration =
      e2e::load_role_configuration<e2e::session_configuration_t> (app, argc, argv);
    app.logging ().use_file (configuration.log_dir + "/" + configuration.node_rid + ".log")
      .set_min_level (fw::log_level_t::debug);
    app.add_zlink_framework ([configuration] (fw::zlink_framework_options_t &framework) {
        framework.services ().add_singleton<session_evidence_store_t> ();
        framework.services ().add_singleton<e2e::session_configuration_t> (
          std::make_unique<e2e::session_configuration_t> (configuration));
        add_redis_location_store (framework, configuration.redis);
        auto mesh = framework.add_route_mesh (e2e::spot_mesh_name)
          .listen (configuration.spot_endpoint)
          .set_routing_id (zlink::routing_id_t::from (configuration.node_rid));
        mesh.peer_connections ().connect (
          zlink::routing_id_t::from (configuration.actor_rid),
          configuration.actor_spot_endpoint);
        framework.add_stream_node (std::string (e2e::spot_mesh_name) + "-" + configuration.node_rid)
          .bind (configuration.stream_endpoint)
          .register_session<actor_session_t> ();
        framework.http ()
          .listen (configuration.http_endpoint)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait");
    });
    return app.run (argc, argv);
}
