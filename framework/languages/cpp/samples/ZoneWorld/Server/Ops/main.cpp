/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/maintenance_store.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

inline bool adjacent (const std::string &left, const std::string &right)
{
    const auto values = adjacent_zones (left);
    return std::find (values.begin (), values.end (), right) != values.end ();
}

class ops_console_registry_t
{
  public:
    void add (const fw::stream_t &stream)
    {
        std::lock_guard lock (_mutex);
        _streams[stream.session_id ()] = stream;
    }

    void remove (const std::string &session_id)
    {
        std::lock_guard lock (_mutex);
        _streams.erase (session_id);
    }

    fw::task_t<void> publish (const node_view_t &node)
    {
        co_await send (node_status_notify_t{node.node_id, node.registered, node.connected,
                                            node.maintenance, node.zones, node.player_count});
    }

    fw::task_t<void> publish_alert (const node_alert_notify_t &alert) { co_await send (alert); }

  private:
    template <typename T> fw::task_t<void> send (const T &message)
    {
        std::vector<fw::stream_t> streams;
        {
            std::lock_guard lock (_mutex);
            for (const auto &[_, stream] : _streams)
                streams.push_back (stream);
        }
        std::vector<fw::task_t<void>> writes;
        writes.reserve (streams.size ());
        for (auto &stream : streams) {
            try {
                writes.push_back (stream.write_packet (zlink::message_t::from_json (message))
                                    .packet_name (T::packet_name)
                                    .timeout (std::chrono::seconds (2))
                                    .submit ());
            }
            catch (...) {
            }
        }
        for (auto &write : writes) {
            try {
                co_await write;
            }
            catch (...) {
            }
        }
    }

    std::mutex _mutex;
    std::map<std::string, fw::stream_t> _streams;
};

class ops_notification_queue_t
{
  public:
    void push (node_view_t node)
    {
        std::lock_guard lock (_mutex);
        _pending.push_back (std::move (node));
    }

    std::vector<node_view_t> take ()
    {
        std::lock_guard lock (_mutex);
        auto pending = std::move (_pending);
        _pending.clear ();
        return pending;
    }

  private:
    std::mutex _mutex;
    std::vector<node_view_t> _pending;
};

class ops_state_t
{
  public:
    node_view_t report (const report_node_status_msg_t &report, const std::string &source_rid)
    {
        std::lock_guard lock (_mutex);
        if (const auto old = _rid_by_node.find (report.node_id);
            old != _rid_by_node.end () && old->second != source_rid)
            _node_by_rid.erase (old->second);
        _rid_by_node[report.node_id] = source_rid;
        _node_by_rid[source_rid] = report.node_id;
        _last_report[report.node_id] = std::chrono::steady_clock::now ();
        auto &node = _nodes[report.node_id];
        node = {report.node_id,     true,         _live_rids.contains (source_rid),
                report.maintenance, report.zones, report.player_count};
        std::sort (node.zones.begin (), node.zones.end ());
        return node;
    }

    std::vector<node_view_t> apply_live_rids (std::set<std::string> live)
    {
        std::lock_guard lock (_mutex);
        _live_rids = std::move (live);
        std::vector<node_view_t> changed;
        for (auto &[node_id, node] : _nodes) {
            const auto found = _rid_by_node.find (node_id);
            const auto connected =
              found != _rid_by_node.end () && _live_rids.contains (found->second);
            if (connected != node.connected) {
                node.connected = connected;
                changed.push_back (node);
            }
        }
        return changed;
    }

    std::vector<node_view_t> expire_reports ()
    {
        std::lock_guard lock (_mutex);
        const auto now = std::chrono::steady_clock::now ();
        std::vector<node_view_t> changed;
        for (auto &[node_id, node] : _nodes) {
            const auto found = _last_report.find (node_id);
            if (!node.registered || found == _last_report.end ()
                || now - found->second < std::chrono::milliseconds (spec_t::report_ttl_ms))
                continue;
            node.registered = false;
            changed.push_back (node);
        }
        return changed;
    }

    std::vector<node_view_t> nodes () const
    {
        std::lock_guard lock (_mutex);
        std::vector<node_view_t> result;
        for (const auto &[_, node] : _nodes)
            result.push_back (node);
        return result;
    }

    std::optional<node_view_t> find (const std::string &node_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _nodes.find (node_id);
        return found == _nodes.end () ? std::nullopt : std::optional<node_view_t> (found->second);
    }

    std::optional<node_view_t> set_maintenance (const std::string &node_id, bool enabled)
    {
        std::lock_guard lock (_mutex);
        const auto found = _nodes.find (node_id);
        if (found == _nodes.end ())
            return std::nullopt;
        found->second.maintenance = enabled;
        return found->second;
    }

    relocation_pair_res_t relocation_pair () const
    {
        std::lock_guard lock (_mutex);
        // The canonical player spawn is logical zone-nw. Discover its current
        // physical owner from reports and select an adjacent zone on another
        // owner; never turn the stable NodeId into a placement pin.
        for (const auto &[source_id, source] : _nodes) {
            if (!source.registered
                || std::find (source.zones.begin (), source.zones.end (), "zone-nw")
                     == source.zones.end ())
                continue;
            for (const auto &source_zone : {std::string ("zone-nw")}) {
                for (const auto &[target_id, target] : _nodes) {
                    if (source_id == target_id || !target.registered)
                        continue;
                    for (const auto &target_zone : target.zones) {
                        if (!adjacent (source_zone, target_zone))
                            continue;
                        const auto source_rid = _rid_by_node.find (source_id);
                        const auto target_rid = _rid_by_node.find (target_id);
                        if (source_rid == _rid_by_node.end () || target_rid == _rid_by_node.end ())
                            continue;
                        return {source_zone, target_zone, source_rid->second, target_rid->second,
                                std::nullopt};
                    }
                }
            }
        }
        return {{}, {}, {}, {}, errors_t::unavailable};
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, node_view_t> _nodes;
    std::map<std::string, std::string> _rid_by_node;
    std::map<std::string, std::string> _node_by_rid;
    std::map<std::string, std::chrono::steady_clock::time_point> _last_report;
    std::set<std::string> _live_rids;
};

class report_node_status_handler_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<ops_state_t, ops_console_registry_t, ops_notification_queue_t>;
    report_node_status_handler_t (ops_state_t &state,
                                  ops_console_registry_t &consoles,
                                  ops_notification_queue_t &notifications) :
        _state (state), _consoles (consoles), _notifications (notifications)
    {
    }
    fw::task_t<void> handle (const report_node_status_msg_t &report,
                             const fw::route_message_context_t &context)
    {
        const auto rid = context.source_node_rid.to_string ();
        const auto node = _state.report (report, rid);
        std::cout << "zoneworld-node-report node=" << node.node_id << " rid=" << rid
                  << " registered=true" << std::endl;
        const auto pending_notifications = _notifications.take ();
        for (const auto &pending : pending_notifications)
            co_await _consoles.publish (pending);
        co_await _consoles.publish (node);
    }

  private:
    ops_state_t &_state;
    ops_console_registry_t &_consoles;
    ops_notification_queue_t &_notifications;
};

class report_spot_event_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<ops_console_registry_t>;
    explicit report_spot_event_handler_t (ops_console_registry_t &consoles) : _consoles (consoles)
    {
    }
    fw::task_t<void> handle (const report_spot_event_msg_t &report)
    {
        std::cout << "zoneworld-node-alert node=" << report.node_id << " kind=" << report.kind
                  << " " << report.detail << std::endl;
        co_await _consoles.publish_alert (
          {report.node_id, report.kind, report.detail, report.occurred_at});
    }

  private:
    ops_console_registry_t &_consoles;
};

class ops_monitor_service_t final : public fw::hosted_service_t
{
  public:
    void start (fw::service_provider_t &services) override
    {
        _state = &services.get_required<ops_state_t> ();
        _notifications = &services.get_required<ops_notification_queue_t> ();
        auto &runtime = services.get_required<fw::route_mesh_runtime_t> ();
        _observation = runtime.observe (
          names_t::mesh, 64,
          [this] (const fw::observed_status_t<fw::mesh_node_snapshot_t> &observed) {
              apply_snapshot (observed.status);
          });
        try {
            apply_snapshot (runtime.snapshot (names_t::mesh));
        }
        catch (...) {
        }
        _running.store (true);
        _worker = std::thread ([this] {
            while (_running.load ()) {
                std::this_thread::sleep_for (std::chrono::milliseconds (250));
                auto expired = _state->expire_reports ();
                for (auto &node : expired) {
                    if (!node.registered)
                        std::cout << "zoneworld-node-expired node=" << node.node_id << std::endl;
                    _notifications->push (std::move (node));
                }
            }
        });
    }

    void request_stop () noexcept override
    {
        _running.store (false);
        if (_observation)
            _observation->close ();
    }

    void stop () noexcept override
    {
        request_stop ();
        if (_worker.joinable ())
            _worker.join ();
        _observation.reset ();
    }

  private:
    void apply_snapshot (const fw::mesh_node_snapshot_t &snapshot)
    {
        std::set<std::string> live;
        for (const auto &peer : snapshot.peers)
            if (peer.state == fw::peer_state_t::ready)
                live.insert (peer.node_rid.to_string ());
        for (const auto &node : _state->apply_live_rids (std::move (live))) {
            std::cout << "zoneworld-node-connected node=" << node.node_id
                      << " connected=" << (node.connected ? "true" : "false") << std::endl;
            _notifications->push (node);
        }
    }

    ops_state_t *_state = nullptr;
    ops_notification_queue_t *_notifications = nullptr;
    std::atomic_bool _running{false};
    std::thread _worker;
    std::unique_ptr<fw::mesh_runtime_observation_t> _observation;
};

class ops_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types = fw::dependency_list_t<ops_state_t,
                                                   ops_console_registry_t,
                                                   fw::publisher_t,
                                                   fw::route_client_t,
                                                   maintenance_store_t>;

    ops_session_t (ops_state_t &state,
                   ops_console_registry_t &consoles,
                   fw::publisher_t &publisher,
                   fw::route_client_t &routes,
                   maintenance_store_t &maintenance) :
        _state (state),
        _consoles (consoles),
        _publisher (publisher),
        _routes (routes),
        _maintenance (maintenance)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &stream) override
    {
        _consoles.add (stream);
        co_return;
    }
    fw::task_t<void> on_disconnected (fw::stream_t &stream) override
    {
        _consoles.remove (stream.session_id ());
        co_return;
    }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);
        if (packet == watch_nodes_req_t::packet_name) {
            stream.reply_packet (zlink::message_t::from_json (watch_nodes_res_t{_state.nodes ()}))
              .submit ();
            co_return;
        }
        if (packet == relocation_pair_req_t::packet_name) {
            stream.reply_packet (zlink::message_t::from_json (_state.relocation_pair ())).submit ();
            co_return;
        }
        if (packet == announce_world_req_t::packet_name) {
            const auto request = payload.parse_json<announce_world_req_t> ();
            const auto id = "announce-" + std::to_string (++_announcement);
            co_await _publisher
              .publish (names_t::broadcast_channel, names_t::announce_topic,
                        world_announce_event_t{id, request.text})
              .submit ();
            stream.reply_packet (zlink::message_t::from_json (announce_world_res_t{id})).submit ();
            co_return;
        }
        if (packet == set_maintenance_req_t::packet_name) {
            const auto request = payload.parse_json<set_maintenance_req_t> ();
            try {
                _maintenance.write (request.node_id, request.enabled);
                const auto applied = co_await _routes
                                       .request_to_channel (names_t::ops_channel (request.node_id),
                                                            apply_node_maintenance_req_t{
                                                              request.node_id, request.enabled})
                                       .timeout (std::chrono::seconds (10))
                                       .submit<apply_node_maintenance_res_t> ();
                co_await _publisher
                  .publish (names_t::broadcast_channel, names_t::maintenance_topic,
                            node_maintenance_changed_event_t{request.node_id, request.enabled})
                  .submit ();
                if (const auto changed = _state.set_maintenance (request.node_id, request.enabled))
                    co_await _consoles.publish (*changed);
                stream
                  .reply_packet (zlink::message_t::from_json (set_maintenance_res_t{
                    applied.node_id, applied.enabled, applied.zones, std::nullopt}))
                  .submit ();
            }
            catch (const fw::framework_exception_t &error) {
                stream
                  .reply_packet (zlink::message_t::from_json (set_maintenance_res_t{
                    request.node_id,
                    request.enabled,
                    {},
                    error.kind () == fw::framework_error_kind_t::deadline_exceeded
                      ? errors_t::deadline_exceeded
                      : errors_t::unavailable}))
                  .submit ();
            }
            co_return;
        }
        if (packet == node_diagnostics_req_t::packet_name) {
            const auto request = payload.parse_json<node_diagnostics_req_t> ();
            try {
                const auto result =
                  co_await _routes
                    .request_to_channel (names_t::ops_channel (request.node_id),
                                         get_node_diagnostics_req_t{request.node_id})
                    .timeout (std::chrono::seconds (10))
                    .submit<get_node_diagnostics_res_t> ();
                stream
                  .reply_packet (zlink::message_t::from_json (
                    node_diagnostics_res_t{result.node_id, result.zones, result.player_count,
                                           result.maintenance, std::nullopt}))
                  .submit ();
            }
            catch (const fw::framework_exception_t &) {
                stream
                  .reply_packet (zlink::message_t::from_json (
                    node_diagnostics_res_t{request.node_id, {}, 0, false, errors_t::unavailable}))
                  .submit ();
            }
            co_return;
        }
        throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                         "unsupported Ops packet: " + packet);
    }

  private:
    ops_state_t &_state;
    ops_console_registry_t &_consoles;
    fw::publisher_t &_publisher;
    fw::route_client_t &_routes;
    maintenance_store_t &_maintenance;
    std::atomic_uint64_t _announcement{0};
};

} // namespace zlink::samples::zoneworld

int main (int argc, char **argv)
{
    using namespace zlink::samples::zoneworld;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    const auto configuration = load_configuration (app, argc, argv);
    app.logging ().use_console ().set_min_level (fw::log_level_t::info);
    auto &options = app.add_zlink_framework ();
    options.services ()
      .add_singleton<ops_state_t> ()
      .add_singleton<ops_console_registry_t> ()
      .add_singleton<ops_notification_queue_t> ()
      .add_singleton<maintenance_store_t> (std::make_unique<maintenance_store_t> (configuration));
    options.add_location_store<fw::redis::redis_location_store_t> ()
      .set_connection_string (configuration.redis_endpoint)
      .set_key_prefix (configuration.redis_key_prefix + "location:");
    options.add_relocation_store<fw::redis::redis_relocation_store_t> ()
      .set_connection_string (configuration.redis_endpoint)
      .set_key_prefix (configuration.redis_key_prefix + "relocation:");
    options.add_fanout_channel (names_t::broadcast_channel)
      .set_routing_id (zlink::routing_id_t::from ("zoneworld-ops-publisher"))
      .enable_publisher (configuration.broadcast_endpoint);
    auto mesh = options.add_route_mesh (names_t::mesh);
    mesh.set_automatic_routing_id_prefix ("ops").listen (configuration.mesh_endpoint);
    mesh.objects ().client ();
    mesh.channel (names_t::report_channel)
      .server ()
      .add_send_handler<report_node_status_handler_t, report_node_status_msg_t> ()
      .add_send_handler<report_spot_event_handler_t, report_spot_event_msg_t> ();
    mesh.channel (names_t::ops_channel ("zone-node-1")).client ();
    mesh.channel (names_t::ops_channel ("zone-node-2")).client ();
    options.add_stream_node (names_t::ops_stream)
      .bind (configuration.stream_endpoint)
      .register_session<ops_session_t> ();
    app.add_hosted_service (std::make_unique<ops_monitor_service_t> ());
    std::cout << "zoneworld-role-ready role=ops\n";
    return app.run (argc, argv);
}
