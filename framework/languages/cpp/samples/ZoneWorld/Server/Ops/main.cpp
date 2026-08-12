/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <atomic>
#include <iostream>
#include <map>
#include <mutex>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

class ops_state_t
{
  public:
    ops_state_t ()
    {
        _nodes.emplace ("zone-node-1", node_view_t{"zone-node-1", true, true, false,
                                                   {"zone-nw", "zone-sw"}, 0});
        _nodes.emplace ("zone-node-2", node_view_t{"zone-node-2", true, true, false,
                                                   {"zone-ne", "zone-se"}, 0});
    }
    std::vector<node_view_t> nodes () const
    { std::lock_guard lock (_mutex); std::vector<node_view_t> result; for (const auto &[_, n] : _nodes) result.push_back (n); return result; }
    std::optional<node_view_t> set_maintenance (const std::string &id, bool enabled)
    { std::lock_guard lock (_mutex); auto found = _nodes.find (id); if (found == _nodes.end ()) return std::nullopt; found->second.maintenance = enabled; return found->second; }
    std::optional<node_view_t> find (const std::string &id) const
    { std::lock_guard lock (_mutex); auto found = _nodes.find (id); return found == _nodes.end () ? std::nullopt : std::optional<node_view_t> (found->second); }
  private:
    mutable std::mutex _mutex;
    std::map<std::string, node_view_t> _nodes;
};

class ops_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types = fw::dependency_list_t<ops_state_t, fw::publisher_t>;
    ops_session_t (ops_state_t &state, fw::publisher_t &publisher) : _state (state), _publisher (publisher) {}
    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream, const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);
        if (packet == watch_nodes_req_t::packet_name) {
            stream.reply_packet (zlink::message_t::from_json (watch_nodes_res_t{_state.nodes ()})).submit ();
            co_return;
        }
        if (packet == announce_world_req_t::packet_name) {
            const auto request = payload.parse_json<announce_world_req_t> ();
            const auto id = "announce-" + std::to_string (++_announcement);
            _publisher.publish (names_t::broadcast_channel, names_t::announce_topic,
                                world_announce_event_t{id, request.text}).submit ();
            stream.reply_packet (zlink::message_t::from_json (announce_world_res_t{id})).submit ();
            co_return;
        }
        if (packet == set_maintenance_req_t::packet_name) {
            const auto request = payload.parse_json<set_maintenance_req_t> ();
            const auto node = _state.set_maintenance (request.node_id, request.enabled);
            if (!node) {
                stream.reply_packet (zlink::message_t::from_json (
                  set_maintenance_res_t{request.node_id, request.enabled, {}, "UnknownNode"})).submit ();
                co_return;
            }
            _publisher.publish (names_t::broadcast_channel, names_t::maintenance_topic,
                                node_maintenance_changed_event_t{request.node_id, request.enabled}).submit ();
            stream.reply_packet (zlink::message_t::from_json (
              set_maintenance_res_t{node->node_id, node->maintenance, node->zones, std::nullopt})).submit ();
            co_return;
        }
        if (packet == node_diagnostics_req_t::packet_name) {
            const auto request = payload.parse_json<node_diagnostics_req_t> ();
            const auto node = _state.find (request.node_id);
            const auto response = node
              ? node_diagnostics_res_t{node->node_id, node->zones, node->player_count,
                                       node->maintenance, std::nullopt}
              : node_diagnostics_res_t{request.node_id, {}, 0, false, "UnknownNode"};
            stream.reply_packet (zlink::message_t::from_json (response)).submit ();
            co_return;
        }
        throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                         "unsupported Ops packet: " + packet);
    }
  private:
    ops_state_t &_state;
    fw::publisher_t &_publisher;
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
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        options.services ().add_singleton<ops_state_t> (std::make_unique<ops_state_t> ());
        add_stores (options, configuration);
        options.add_fanout_channel (names_t::broadcast_channel)
          .set_routing_id (zlink::routing_id_t::from ("zoneworld-ops-publisher"))
          .enable_publisher (configuration.broadcast_endpoint);
        auto mesh = options.add_route_mesh (names_t::mesh);
        mesh.set_automatic_routing_id_prefix ("ops").set_object_role (fw::object_role_t::client);
        mesh.channel_name (names_t::report_channel).server ();
        mesh.listen (configuration.mesh_endpoint);
        options.add_stream_node (names_t::ops_stream)
          .bind (configuration.stream_endpoint).register_session<ops_session_t> ();
    });
    std::cout << "zoneworld-role-ready role=ops\n";
    return app.run (argc, argv);
}
