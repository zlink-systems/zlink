/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nlohmann
{
template <typename T> struct adl_serializer<std::optional<T>>
{
    static void to_json (json &value, const std::optional<T> &input)
    {
        value = input ? json (*input) : json (nullptr);
    }
    static void from_json (const json &value, std::optional<T> &output)
    {
        output = value.is_null () ? std::nullopt : std::optional<T> (value.get<T> ());
    }
};
} // namespace nlohmann

namespace zlink::samples::zoneworld
{

#define ZW_PACKET(name, wire) static constexpr const char *packet_name = wire

struct player_view_t
{
    std::string player_id;
    int x = 0;
    int y = 0;
    std::string zone_id;
    bool is_bot = false;
};

struct node_view_t
{
    std::string node_id;
    bool registered = false;
    bool connected = false;
    bool maintenance = false;
    std::vector<std::string> zones;
    int player_count = 0;
};

struct join_world_req_t { ZW_PACKET (join_world_req_t, "JoinWorldReq"); std::string player_id; };
struct join_world_res_t { ZW_PACKET (join_world_res_t, "JoinWorldRes"); std::string player_id; std::string zone_id; int x = 0; int y = 0; std::optional<std::string> error; };
struct move_msg_t { ZW_PACKET (move_msg_t, "MoveMsg"); int x = 0; int y = 0; };
struct zone_state_notify_t { ZW_PACKET (zone_state_notify_t, "ZoneStateNotify"); std::string zone_id; std::int64_t tick = 0; std::vector<player_view_t> players; };
struct zone_changed_notify_t { ZW_PACKET (zone_changed_notify_t, "ZoneChangedNotify"); std::string player_id; std::string zone_id; };
struct world_announce_notify_t { ZW_PACKET (world_announce_notify_t, "WorldAnnounceNotify"); std::string announcement_id; std::string text; };
struct move_rejected_notify_t { ZW_PACKET (move_rejected_notify_t, "MoveRejectedNotify"); std::string reason; int x = 0; int y = 0; };

struct watch_nodes_req_t { ZW_PACKET (watch_nodes_req_t, "WatchNodesReq"); };
struct watch_nodes_res_t { ZW_PACKET (watch_nodes_res_t, "WatchNodesRes"); std::vector<node_view_t> nodes; };
struct node_status_notify_t { ZW_PACKET (node_status_notify_t, "NodeStatusNotify"); std::string node_id; bool registered = false; bool connected = false; bool maintenance = false; std::vector<std::string> zones; int player_count = 0; };
struct node_alert_notify_t { ZW_PACKET (node_alert_notify_t, "NodeAlertNotify"); std::string node_id; std::string kind; std::string detail; std::string occurred_at; };
struct announce_world_req_t { ZW_PACKET (announce_world_req_t, "AnnounceWorldReq"); std::string text; };
struct announce_world_res_t { ZW_PACKET (announce_world_res_t, "AnnounceWorldRes"); std::string announcement_id; };
struct set_maintenance_req_t { ZW_PACKET (set_maintenance_req_t, "SetMaintenanceReq"); std::string node_id; bool enabled = false; };
struct set_maintenance_res_t { ZW_PACKET (set_maintenance_res_t, "SetMaintenanceRes"); std::string node_id; bool enabled = false; std::vector<std::string> zones; std::optional<std::string> error; };
struct node_diagnostics_req_t { ZW_PACKET (node_diagnostics_req_t, "NodeDiagnosticsReq"); std::string node_id; };
struct node_diagnostics_res_t { ZW_PACKET (node_diagnostics_res_t, "NodeDiagnosticsRes"); std::string node_id; std::vector<std::string> zones; int player_count = 0; bool maintenance = false; std::optional<std::string> error; };
struct relocation_pair_req_t { ZW_PACKET (relocation_pair_req_t, "RelocationPairReq"); };
struct relocation_pair_res_t { ZW_PACKET (relocation_pair_res_t, "RelocationPairRes"); std::string source_zone_id; std::string target_zone_id; std::string source_owner_node_rid; std::string target_owner_node_rid; std::optional<std::string> error; };
struct actor_location_probe_req_t { ZW_PACKET (actor_location_probe_req_t, "ActorLocationProbeReq"); std::string actor_id; };
struct actor_location_probe_res_t { ZW_PACKET (actor_location_probe_res_t, "ActorLocationProbeRes"); std::string actor_id; std::uint64_t object_generation = 0; std::string owner_node_rid; std::optional<std::string> error; };
struct fresh_actor_probe_req_t { ZW_PACKET (fresh_actor_probe_req_t, "FreshActorProbeReq"); std::string actor_id; };
struct fresh_actor_probe_res_t { ZW_PACKET (fresh_actor_probe_res_t, "FreshActorProbeRes"); std::string actor_id; std::uint64_t object_generation = 0; std::string owner_node_rid; std::optional<std::string> error; };

struct world_announce_event_t { ZW_PACKET (world_announce_event_t, "WorldAnnounceEvent"); std::string announcement_id; std::string text; };
struct node_maintenance_changed_event_t { ZW_PACKET (node_maintenance_changed_event_t, "NodeMaintenanceChangedEvent"); std::string node_id; bool enabled = false; };
struct deliver_announce_msg_t { ZW_PACKET (deliver_announce_msg_t, "DeliverAnnounceMsg"); std::string announcement_id; std::string text; };
struct bot_tick_msg_t { ZW_PACKET (bot_tick_msg_t, "BotTickMsg"); };
struct enter_world_req_t { ZW_PACKET (enter_world_req_t, "EnterWorldReq"); int x = 0; int y = 0; bool is_bot = false; std::optional<int> dir_x; std::optional<int> dir_y; };
struct enter_world_res_t { ZW_PACKET (enter_world_res_t, "EnterWorldRes"); std::string zone_id; int x = 0; int y = 0; std::optional<std::string> error; };
struct apply_node_maintenance_req_t { ZW_PACKET (apply_node_maintenance_req_t, "ApplyNodeMaintenanceReq"); std::string node_id; bool enabled = false; };
struct apply_node_maintenance_res_t { ZW_PACKET (apply_node_maintenance_res_t, "ApplyNodeMaintenanceRes"); std::string node_id; bool enabled = false; std::vector<std::string> zones; };
struct get_node_diagnostics_req_t { ZW_PACKET (get_node_diagnostics_req_t, "GetNodeDiagnosticsReq"); std::string node_id; };
struct get_node_diagnostics_res_t { ZW_PACKET (get_node_diagnostics_res_t, "GetNodeDiagnosticsRes"); std::string node_id; std::vector<std::string> zones; int player_count = 0; bool maintenance = false; };
struct report_spot_event_msg_t { ZW_PACKET (report_spot_event_msg_t, "ReportSpotEventMsg"); std::string node_id; std::string kind; std::string detail; std::string occurred_at; };
struct report_node_status_msg_t { ZW_PACKET (report_node_status_msg_t, "ReportNodeStatusMsg"); std::string node_id; std::vector<std::string> zones; int player_count = 0; bool maintenance = false; };
struct zone_border_event_t { ZW_PACKET (zone_border_event_t, "ZoneBorderEvent"); std::string from_zone_id; std::string to_zone_id; std::int64_t tick = 0; std::vector<player_view_t> players; };
struct enter_zone_req_t { ZW_PACKET (enter_zone_req_t, "EnterZoneReq"); std::string player_id; int x = 0; int y = 0; bool is_bot = false; bool initial_entry = false; std::optional<std::string> from_zone_id; bool crash_boundary_probe = false; };
struct enter_zone_res_t { ZW_PACKET (enter_zone_res_t, "EnterZoneRes"); std::string zone_id; std::optional<std::string> error; };
struct update_position_msg_t { ZW_PACKET (update_position_msg_t, "UpdatePositionMsg"); std::string player_id; int x = 0; int y = 0; bool is_bot = false; };
struct deliver_zone_state_msg_t { ZW_PACKET (deliver_zone_state_msg_t, "DeliverZoneStateMsg"); std::string zone_id; std::int64_t tick = 0; std::vector<player_view_t> players; };
struct deliver_zone_changed_msg_t { ZW_PACKET (deliver_zone_changed_msg_t, "DeliverZoneChangedMsg"); std::string player_id; std::string zone_id; };
struct deliver_world_announce_msg_t { ZW_PACKET (deliver_world_announce_msg_t, "DeliverWorldAnnounceMsg"); std::string announcement_id; std::string text; };
struct message_follow_probe_req_t { ZW_PACKET (message_follow_probe_req_t, "MessageFollowProbeReq"); std::string actor_id; std::string probe_id; std::vector<std::uint8_t> payload; };
struct message_follow_probe_msg_t { ZW_PACKET (message_follow_probe_msg_t, "MessageFollowProbeMsg"); std::string actor_id; std::string probe_id; std::vector<std::uint8_t> payload; };
struct message_follow_probe_res_t { ZW_PACKET (message_follow_probe_res_t, "MessageFollowProbeRes"); std::string probe_id; std::vector<std::uint8_t> payload; std::optional<std::string> error; };
struct crash_relocation_probe_msg_t { ZW_PACKET (crash_relocation_probe_msg_t, "CrashRelocationProbeMsg"); int x = 0; int y = 0; };
struct crash_relocation_probe_res_t { ZW_PACKET (crash_relocation_probe_res_t, "CrashRelocationProbeRes"); std::optional<std::string> error; };

#define ZW_WRITE(field, wire) j[wire] = value.field;
#define ZW_READ(field, wire) j.at (wire).get_to (value.field);
#define ZW_JSON_FIELDS(type, fields)                                                   \
    inline void to_json (nlohmann::json &j, const type &value)                        \
    { j = nlohmann::json::object (); fields (ZW_WRITE) }                              \
    inline void from_json (const nlohmann::json &j, type &value)                      \
    { fields (ZW_READ) }
#define ZW_EMPTY_JSON(type)                                                            \
    inline void to_json (nlohmann::json &j, const type &) { j = nlohmann::json::object (); } \
    inline void from_json (const nlohmann::json &, type &) {}

#define PLAYER_VIEW_FIELDS(F) F (player_id, "playerId") F (x, "x") F (y, "y") F (zone_id, "zoneId") F (is_bot, "isBot")
#define NODE_VIEW_FIELDS(F) F (node_id, "nodeId") F (registered, "registered") F (connected, "connected") F (maintenance, "maintenance") F (zones, "zones") F (player_count, "playerCount")
#define JOIN_WORLD_REQ_FIELDS(F) F (player_id, "playerId")
#define JOIN_WORLD_RES_FIELDS(F) F (player_id, "playerId") F (zone_id, "zoneId") F (x, "x") F (y, "y") F (error, "error")
#define MOVE_FIELDS(F) F (x, "x") F (y, "y")
#define ZONE_STATE_FIELDS(F) F (zone_id, "zoneId") F (tick, "tick") F (players, "players")
#define ZONE_CHANGED_FIELDS(F) F (player_id, "playerId") F (zone_id, "zoneId")
#define ANNOUNCE_FIELDS(F) F (announcement_id, "announcementId") F (text, "text")
#define MOVE_REJECTED_FIELDS(F) F (reason, "reason") F (x, "x") F (y, "y")
#define NODES_FIELDS(F) F (nodes, "nodes")
#define NODE_ALERT_FIELDS(F) F (node_id, "nodeId") F (kind, "kind") F (detail, "detail") F (occurred_at, "occurredAt")
#define TEXT_FIELDS(F) F (text, "text")
#define ANNOUNCEMENT_ID_FIELDS(F) F (announcement_id, "announcementId")
#define MAINTENANCE_REQ_FIELDS(F) F (node_id, "nodeId") F (enabled, "enabled")
#define MAINTENANCE_RES_FIELDS(F) F (node_id, "nodeId") F (enabled, "enabled") F (zones, "zones") F (error, "error")
#define NODE_ID_FIELDS(F) F (node_id, "nodeId")
#define DIAGNOSTICS_FIELDS(F) F (node_id, "nodeId") F (zones, "zones") F (player_count, "playerCount") F (maintenance, "maintenance") F (error, "error")
#define RELOCATION_PAIR_FIELDS(F) F (source_zone_id, "sourceZoneId") F (target_zone_id, "targetZoneId") F (source_owner_node_rid, "sourceOwnerNodeRid") F (target_owner_node_rid, "targetOwnerNodeRid") F (error, "error")
#define ACTOR_LOCATION_REQ_FIELDS(F) F (actor_id, "actorId")
#define ACTOR_LOCATION_RES_FIELDS(F) F (actor_id, "actorId") F (object_generation, "objectGeneration") F (owner_node_rid, "ownerNodeRid") F (error, "error")
#define ENTER_WORLD_REQ_FIELDS(F) F (x, "x") F (y, "y") F (is_bot, "isBot") F (dir_x, "dirX") F (dir_y, "dirY")
#define ENTER_WORLD_RES_FIELDS(F) F (zone_id, "zoneId") F (x, "x") F (y, "y") F (error, "error")
#define APPLY_MAINTENANCE_FIELDS(F) F (node_id, "nodeId") F (enabled, "enabled") F (zones, "zones")
#define GET_DIAGNOSTICS_FIELDS(F) F (node_id, "nodeId") F (zones, "zones") F (player_count, "playerCount") F (maintenance, "maintenance")
#define REPORT_EVENT_FIELDS(F) F (node_id, "nodeId") F (kind, "kind") F (detail, "detail") F (occurred_at, "occurredAt")
#define REPORT_STATUS_FIELDS(F) F (node_id, "nodeId") F (zones, "zones") F (player_count, "playerCount") F (maintenance, "maintenance")
#define BORDER_FIELDS(F) F (from_zone_id, "fromZoneId") F (to_zone_id, "toZoneId") F (tick, "tick") F (players, "players")
#define ENTER_ZONE_FIELDS(F) F (player_id, "playerId") F (x, "x") F (y, "y") F (is_bot, "isBot") F (initial_entry, "initialEntry") F (from_zone_id, "fromZoneId") F (crash_boundary_probe, "crashBoundaryProbe")
#define ENTER_ZONE_RES_FIELDS(F) F (zone_id, "zoneId") F (error, "error")
#define UPDATE_POSITION_FIELDS(F) F (player_id, "playerId") F (x, "x") F (y, "y") F (is_bot, "isBot")
#define FOLLOW_REQ_FIELDS(F) F (actor_id, "actorId") F (probe_id, "probeId") F (payload, "payload")
#define FOLLOW_RES_FIELDS(F) F (probe_id, "probeId") F (payload, "payload") F (error, "error")
#define CRASH_MOVE_FIELDS(F) F (x, "x") F (y, "y")
#define ERROR_FIELDS(F) F (error, "error")

ZW_JSON_FIELDS (player_view_t, PLAYER_VIEW_FIELDS)
ZW_JSON_FIELDS (node_view_t, NODE_VIEW_FIELDS)
ZW_JSON_FIELDS (join_world_req_t, JOIN_WORLD_REQ_FIELDS)
ZW_JSON_FIELDS (join_world_res_t, JOIN_WORLD_RES_FIELDS)
ZW_JSON_FIELDS (move_msg_t, MOVE_FIELDS)
ZW_JSON_FIELDS (zone_state_notify_t, ZONE_STATE_FIELDS)
ZW_JSON_FIELDS (zone_changed_notify_t, ZONE_CHANGED_FIELDS)
ZW_JSON_FIELDS (world_announce_notify_t, ANNOUNCE_FIELDS)
ZW_JSON_FIELDS (move_rejected_notify_t, MOVE_REJECTED_FIELDS)
ZW_EMPTY_JSON (watch_nodes_req_t)
ZW_JSON_FIELDS (watch_nodes_res_t, NODES_FIELDS)
ZW_JSON_FIELDS (node_status_notify_t, NODE_VIEW_FIELDS)
ZW_JSON_FIELDS (node_alert_notify_t, NODE_ALERT_FIELDS)
ZW_JSON_FIELDS (announce_world_req_t, TEXT_FIELDS)
ZW_JSON_FIELDS (announce_world_res_t, ANNOUNCEMENT_ID_FIELDS)
ZW_JSON_FIELDS (set_maintenance_req_t, MAINTENANCE_REQ_FIELDS)
ZW_JSON_FIELDS (set_maintenance_res_t, MAINTENANCE_RES_FIELDS)
ZW_JSON_FIELDS (node_diagnostics_req_t, NODE_ID_FIELDS)
ZW_JSON_FIELDS (node_diagnostics_res_t, DIAGNOSTICS_FIELDS)
ZW_EMPTY_JSON (relocation_pair_req_t)
ZW_JSON_FIELDS (relocation_pair_res_t, RELOCATION_PAIR_FIELDS)
ZW_JSON_FIELDS (actor_location_probe_req_t, ACTOR_LOCATION_REQ_FIELDS)
ZW_JSON_FIELDS (actor_location_probe_res_t, ACTOR_LOCATION_RES_FIELDS)
ZW_JSON_FIELDS (fresh_actor_probe_req_t, ACTOR_LOCATION_REQ_FIELDS)
ZW_JSON_FIELDS (fresh_actor_probe_res_t, ACTOR_LOCATION_RES_FIELDS)
ZW_JSON_FIELDS (world_announce_event_t, ANNOUNCE_FIELDS)
ZW_JSON_FIELDS (node_maintenance_changed_event_t, MAINTENANCE_REQ_FIELDS)
ZW_JSON_FIELDS (deliver_announce_msg_t, ANNOUNCE_FIELDS)
ZW_EMPTY_JSON (bot_tick_msg_t)
ZW_JSON_FIELDS (enter_world_req_t, ENTER_WORLD_REQ_FIELDS)
ZW_JSON_FIELDS (enter_world_res_t, ENTER_WORLD_RES_FIELDS)
ZW_JSON_FIELDS (apply_node_maintenance_req_t, MAINTENANCE_REQ_FIELDS)
ZW_JSON_FIELDS (apply_node_maintenance_res_t, APPLY_MAINTENANCE_FIELDS)
ZW_JSON_FIELDS (get_node_diagnostics_req_t, NODE_ID_FIELDS)
ZW_JSON_FIELDS (get_node_diagnostics_res_t, GET_DIAGNOSTICS_FIELDS)
ZW_JSON_FIELDS (report_spot_event_msg_t, REPORT_EVENT_FIELDS)
ZW_JSON_FIELDS (report_node_status_msg_t, REPORT_STATUS_FIELDS)
ZW_JSON_FIELDS (zone_border_event_t, BORDER_FIELDS)
ZW_JSON_FIELDS (enter_zone_req_t, ENTER_ZONE_FIELDS)
ZW_JSON_FIELDS (enter_zone_res_t, ENTER_ZONE_RES_FIELDS)
ZW_JSON_FIELDS (update_position_msg_t, UPDATE_POSITION_FIELDS)
ZW_JSON_FIELDS (deliver_zone_state_msg_t, ZONE_STATE_FIELDS)
ZW_JSON_FIELDS (deliver_zone_changed_msg_t, ZONE_CHANGED_FIELDS)
ZW_JSON_FIELDS (deliver_world_announce_msg_t, ANNOUNCE_FIELDS)
ZW_JSON_FIELDS (message_follow_probe_req_t, FOLLOW_REQ_FIELDS)
ZW_JSON_FIELDS (message_follow_probe_msg_t, FOLLOW_REQ_FIELDS)
ZW_JSON_FIELDS (message_follow_probe_res_t, FOLLOW_RES_FIELDS)
ZW_JSON_FIELDS (crash_relocation_probe_msg_t, CRASH_MOVE_FIELDS)
ZW_JSON_FIELDS (crash_relocation_probe_res_t, ERROR_FIELDS)

#undef ERROR_FIELDS
#undef CRASH_MOVE_FIELDS
#undef FOLLOW_RES_FIELDS
#undef FOLLOW_REQ_FIELDS
#undef UPDATE_POSITION_FIELDS
#undef ENTER_ZONE_RES_FIELDS
#undef ENTER_ZONE_FIELDS
#undef BORDER_FIELDS
#undef REPORT_STATUS_FIELDS
#undef REPORT_EVENT_FIELDS
#undef GET_DIAGNOSTICS_FIELDS
#undef APPLY_MAINTENANCE_FIELDS
#undef ENTER_WORLD_RES_FIELDS
#undef ENTER_WORLD_REQ_FIELDS
#undef ACTOR_LOCATION_RES_FIELDS
#undef ACTOR_LOCATION_REQ_FIELDS
#undef RELOCATION_PAIR_FIELDS
#undef DIAGNOSTICS_FIELDS
#undef NODE_ID_FIELDS
#undef MAINTENANCE_RES_FIELDS
#undef MAINTENANCE_REQ_FIELDS
#undef ANNOUNCEMENT_ID_FIELDS
#undef TEXT_FIELDS
#undef NODE_ALERT_FIELDS
#undef NODES_FIELDS
#undef MOVE_REJECTED_FIELDS
#undef ANNOUNCE_FIELDS
#undef ZONE_CHANGED_FIELDS
#undef ZONE_STATE_FIELDS
#undef MOVE_FIELDS
#undef JOIN_WORLD_RES_FIELDS
#undef JOIN_WORLD_REQ_FIELDS
#undef NODE_VIEW_FIELDS
#undef PLAYER_VIEW_FIELDS
#undef ZW_EMPTY_JSON
#undef ZW_JSON_FIELDS
#undef ZW_READ
#undef ZW_WRITE
#undef ZW_PACKET

} // namespace zlink::samples::zoneworld
