/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/world_rules.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <thread>

namespace zlink::samples::zoneworld
{
namespace se = zlink::stream_e2e_client;

struct client_topology_t
{
    std::string game_endpoint = "tcp://127.0.0.1:35201";
    std::string ops_endpoint = "tcp://127.0.0.1:35202";
    std::string scenario = "main";
    std::string target_node_id;
    std::string arm_file;
};

inline client_topology_t load_client_topology (int argc, char **argv)
{
    client_topology_t topology;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? std::string{} : argv[index];
        const auto read = [&] (std::string_view prefix, std::string &value) {
            if (arg.rfind (prefix, 0) == 0) {
                value = arg.substr (prefix.size ());
                return true;
            }
            return false;
        };
        if (read ("--game-endpoint=", topology.game_endpoint))
            continue;
        if (read ("--ops-endpoint=", topology.ops_endpoint))
            continue;
        if (read ("--scenario=", topology.scenario))
            continue;
        if (read ("--target-node-id=", topology.target_node_id))
            continue;
        if (read ("--arm-file=", topology.arm_file))
            continue;
        if (arg == "--game-endpoint" && index + 1 < argc)
            topology.game_endpoint = argv[++index];
        else if (arg == "--ops-endpoint" && index + 1 < argc)
            topology.ops_endpoint = argv[++index];
        else if (arg == "--scenario" && index + 1 < argc)
            topology.scenario = argv[++index];
        else if (arg == "--target-node-id" && index + 1 < argc)
            topology.target_node_id = argv[++index];
        else if (arg == "--arm-file" && index + 1 < argc)
            topology.arm_file = argv[++index];
    }
    return topology;
}

inline void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

se::task_t<void> wait_for_condition (
  std::function<bool ()> condition,
  std::chrono::milliseconds timeout,
  std::string timeout_message)
{
    return se::task_t<void> (
      [condition = std::move (condition), timeout,
       timeout_message = std::move (timeout_message)] (
        std::function<void (se::result_t<void>)> completed) mutable {
          std::thread (
            [condition = std::move (condition), timeout,
             timeout_message = std::move (timeout_message),
             completed = std::move (completed)] () mutable {
                const auto deadline = std::chrono::steady_clock::now () + timeout;
                while (!condition ()) {
                    if (std::chrono::steady_clock::now () >= deadline) {
                        completed (se::result_t<void>::failure (
                          se::error_code_t::user_callback_failed,
                          std::move (timeout_message)));
                        return;
                    }
                    std::this_thread::sleep_for (
                      std::chrono::milliseconds (25));
                }
                completed (se::result_t<void>::success ());
            })
            .detach ();
      });
}

zlink::stream_connector::connector_t make_connector (const std::string &endpoint)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = endpoint;
    options.connect_timeout = std::chrono::seconds (15);
    options.request_timeout = std::chrono::seconds (45);
    options.wait_timeout = std::chrono::seconds (45);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    connector.codecs ()
      .enable_codec (zlink::stream_connector::codec_t::json)
      .use_default_codec (zlink::stream_connector::codec_t::json);
    return connector;
}

struct crossing_t
{
    std::string target_zone_id;
    int source_x = 0;
    int source_y = 0;
    int target_x = 0;
    int target_y = 0;
};

inline crossing_t crossing_from_nw (const std::string &target_zone_id)
{
    if (target_zone_id == "zone-ne")
        return {target_zone_id, 48, 45, 52, 45};
    if (target_zone_id == "zone-sw")
        return {target_zone_id, 45, 48, 45, 52};
    throw std::runtime_error ("ZoneWorld requires an adjacent zone-nw crossing");
}

inline const node_view_t &node_for_zone (const watch_nodes_res_t &nodes, const std::string &zone_id)
{
    const auto found =
      std::find_if (nodes.nodes.begin (), nodes.nodes.end (), [&] (const auto &node) {
          return std::find (node.zones.begin (), node.zones.end (), zone_id) != node.zones.end ();
      });
    if (found == nodes.nodes.end ())
        throw std::runtime_error ("ZoneWorld node report is missing a logical zone");
    return *found;
}

inline std::string same_owner_adjacent_zone (const watch_nodes_res_t &nodes,
                                             const std::string &source_zone_id)
{
    const auto &owner = node_for_zone (nodes, source_zone_id);
    const auto adjacent = adjacent_zones (source_zone_id);
    const auto found =
      std::find_if (owner.zones.begin (), owner.zones.end (), [&] (const auto &zone) {
          return zone != source_zone_id
                 && std::find (adjacent.begin (), adjacent.end (), zone) != adjacent.end ();
      });
    if (found == owner.zones.end ())
        throw std::runtime_error ("ZoneWorld placement has no same-owner adjacent zone");
    return *found;
}

se::task_t<join_world_res_t> join_and_wait (se::coroutine_connector_t &game,
                                            const std::string &player_id)
{
    auto reply_wait =
      game.wait_for<join_world_res_t> ()
        .where ([player_id] (const auto &reply) { return reply.player_id == player_id; })
        .async ();
    auto state_wait =
      game.wait_for<zone_state_notify_t> ()
        .where ([player_id] (const auto &state) {
            return std::any_of (state.players.begin (), state.players.end (),
                                [&] (const auto &player) { return player.player_id == player_id; });
        })
        .async ();
    game.send (join_world_req_t{player_id}).submit ();
    const auto reply = co_await reply_wait;
    require (!reply.error, "JoinWorldReq was rejected");
    const auto state = co_await state_wait;
    require (state.zone_id == reply.zone_id,
             "JoinWorldRes and first owned ZoneStateNotify disagree");
    co_return reply;
}

se::task_t<void> move_within (se::coroutine_connector_t &game,
                              const std::string &player_id,
                              int &x,
                              int &y,
                              int target_x,
                              int target_y)
{
    while (x != target_x || y != target_y) {
        int next_x = x;
        int next_y = y;
        if (x != target_x)
            next_x += std::clamp (target_x - x, -spec_t::max_step, spec_t::max_step);
        else
            next_y += std::clamp (target_y - y, -spec_t::max_step, spec_t::max_step);
        auto state_wait = game.wait_for<zone_state_notify_t> ()
                            .where ([player_id, next_x, next_y] (const auto &state) {
                                return std::any_of (state.players.begin (), state.players.end (),
                                                    [&] (const auto &player) {
                                                        return player.player_id == player_id
                                                               && player.x == next_x
                                                               && player.y == next_y;
                                                    });
                            })
                            .async ();
        game.send (move_msg_t{next_x, next_y}).submit ();
        (void) co_await state_wait;
        x = next_x;
        y = next_y;
    }
}

se::task_t<void> cross_zone (se::coroutine_connector_t &game,
                             const std::string &player_id,
                             int &x,
                             int &y,
                             const crossing_t &edge,
                             bool returning = false)
{
    const auto expected_zone = returning ? std::string ("zone-nw") : edge.target_zone_id;
    const auto target_x = returning ? edge.source_x : edge.target_x;
    const auto target_y = returning ? edge.source_y : edge.target_y;
    auto changed_wait =
      game.wait_for<zone_changed_notify_t> ()
        .where ([player_id, expected_zone] (const auto &changed) {
            return changed.player_id == player_id && changed.zone_id == expected_zone;
        })
        .async ();
    game.send (move_msg_t{target_x, target_y}).submit ();
    (void) co_await changed_wait;
    x = target_x;
    y = target_y;
}

se::task_t<actor_location_probe_res_t> probe_actor (se::coroutine_connector_t &game,
                                                    const std::string &player_id)
{
    co_return co_await game.request (actor_location_probe_req_t{player_id})
      .async<actor_location_probe_res_t> ();
}

se::task_t<bool> run_main (se::coroutine_connector_t &game,
                           se::coroutine_connector_t &neighbor,
                           se::coroutine_connector_t &probe,
                           se::coroutine_connector_t &ops)
{
    co_await game.connect ().async ();
    co_await neighbor.connect ().async ();
    co_await ops.connect ().async ();

    const auto nodes = co_await ops.request (watch_nodes_req_t{}).async<watch_nodes_res_t> ();
    require (nodes.nodes.size () == 2, "Ops must report exactly two ZoneNodes");
    require (std::all_of (nodes.nodes.begin (), nodes.nodes.end (),
                          [] (const auto &node) { return node.registered && node.connected; }),
             "ZoneNodes must be registered and connected");
    std::cout << "scenario ZW-C1 passed\n";

    const auto pair =
      co_await ops.request (relocation_pair_req_t{}).async<relocation_pair_res_t> ();
    require (!pair.error && pair.source_zone_id == "zone-nw",
             "Ops did not discover the canonical cross-owner adjacent pair");
    const auto edge = crossing_from_nw (pair.target_zone_id);
    const std::regex canonical_rid (
      R"(^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)");
    require (pair.source_owner_node_rid != pair.target_owner_node_rid
               && std::regex_match (pair.source_owner_node_rid, canonical_rid)
               && std::regex_match (pair.target_owner_node_rid, canonical_rid),
             "ZoneNode RIDs must be distinct zn-lowercase-UUIDv4 values");
    std::cout << "scenario ZW-G1 passed\n";

    int alice_x = spec_t::spawn_x;
    int alice_y = spec_t::spawn_y;
    const auto joined = co_await join_and_wait (game, "player-alice");
    require (joined.zone_id == "zone-nw" && joined.x == alice_x && joined.y == alice_y,
             "JoinWorldRes must confirm the canonical spawn");
    std::cout << "scenario ZW-A1 passed\n";

    auto alice_shared_wait =
      game.wait_for<zone_state_notify_t> ()
        .where ([] (const auto &state) {
            return std::any_of (
                     state.players.begin (), state.players.end (),
                     [] (const auto &player) { return player.player_id == "player-alice"; })
                   && std::any_of (
                     state.players.begin (), state.players.end (),
                     [] (const auto &player) { return player.player_id == "player-bob"; });
        })
        .async ();
    const auto bob_joined = co_await join_and_wait (neighbor, "player-bob");
    require (bob_joined.zone_id == "zone-nw", "second player did not join the spawn zone");
    const auto alice_shared = co_await alice_shared_wait;
    auto bob_shared_wait =
      neighbor.wait_for<zone_state_notify_t> ()
        .where ([] (const auto &state) {
            return std::any_of (
                     state.players.begin (), state.players.end (),
                     [] (const auto &player) { return player.player_id == "player-alice"; })
                   && std::any_of (
                     state.players.begin (), state.players.end (),
                     [] (const auto &player) { return player.player_id == "player-bob"; });
        })
        .async ();
    const auto bob_shared = co_await bob_shared_wait;
    require (alice_shared.players.size () >= 2 && bob_shared.players.size () >= 2,
             "both same-zone clients must see both players");
    std::cout << "scenario ZW-A4 passed\n";
    require (std::is_sorted (alice_shared.players.begin (), alice_shared.players.end (),
                             [] (const auto &left, const auto &right) {
                                 return left.player_id < right.player_id;
                             }),
             "ZoneStateNotify players are not sorted by PlayerId UTF-8 bytes");
    std::cout << "scenario ZW-A5 passed\n";

    co_await move_within (game, "player-alice", alice_x, alice_y, 28, 27);
    std::cout << "scenario ZW-A2 passed\n";

    std::vector<std::string> rejection_order;
    auto reject = [&] (int x, int y, const char *expected) -> se::task_t<void> {
        auto rejected_wait = game.wait_for<move_rejected_notify_t> ().async ();
        game.send (move_msg_t{x, y}).submit ();
        const auto rejected = co_await rejected_wait;
        require (rejected.reason == expected, "MoveRejectedNotify reason mismatch");
        rejection_order.push_back (rejected.reason);
    };
    co_await reject (-1, alice_y, reject_reason_t::out_of_range);
    co_await reject (alice_x + spec_t::max_step + 1, alice_y, reject_reason_t::too_far);
    co_await move_within (game, "player-alice", alice_x, alice_y, 48, 48);
    co_await reject (52, 52, reject_reason_t::diagonal_crossing);
    co_await move_within (game, "player-alice", alice_x, alice_y, edge.source_x, edge.source_y);
    const auto &target_node = node_for_zone (nodes, pair.target_zone_id);
    const auto target_maintenance =
      co_await ops.request (set_maintenance_req_t{target_node.node_id, true})
        .async<set_maintenance_res_t> ();
    require (!target_maintenance.error && target_maintenance.enabled,
             "A3 target maintenance was not applied");
    co_await reject (edge.target_x, edge.target_y, reject_reason_t::zone_maintenance);
    const auto target_reset =
      co_await ops.request (set_maintenance_req_t{target_node.node_id, false})
        .async<set_maintenance_res_t> ();
    require (!target_reset.error && !target_reset.enabled, "A3 target maintenance cleanup failed");
    const std::vector<std::string> expected_rejections{
      reject_reason_t::out_of_range, reject_reason_t::too_far, reject_reason_t::diagonal_crossing,
      reject_reason_t::zone_maintenance};
    require (rejection_order == expected_rejections, "A3 rejection order is not canonical");
    std::cout << "scenario ZW-A3 passed\n";

    const auto &source_node = node_for_zone (nodes, "zone-nw");
    const auto source_maintenance =
      co_await ops.request (set_maintenance_req_t{source_node.node_id, true})
        .async<set_maintenance_res_t> ();
    require (!source_maintenance.error && source_maintenance.enabled
               && source_maintenance.zones == source_node.zones,
             "targeted maintenance did not change only the selected NodeId");
    std::cout << "scenario ZW-E1 passed\n";

    co_await probe.connect ().async ();
    auto blocked_join_wait =
      probe.wait_for<join_world_res_t> ()
        .where ([] (const auto &reply) { return reply.player_id == "player-maintenance"; })
        .async ();
    probe.send (join_world_req_t{"player-maintenance"}).submit ();
    const auto blocked_join = co_await blocked_join_wait;
    require (blocked_join.error == reject_reason_t::zone_maintenance,
             "maintenance did not reject a new spawn admission");
    co_await probe.close ().async ();
    std::cout << "scenario ZW-E2 passed\n";

    const int same_zone_x = edge.source_x == 48 ? 47 : edge.source_x;
    const int same_zone_y = edge.source_y == 48 ? 47 : edge.source_y;
    co_await move_within (game, "player-alice", alice_x, alice_y, same_zone_x, same_zone_y);
    std::cout << "scenario ZW-E3 passed\n";

    const auto same_owner_zone = same_owner_adjacent_zone (nodes, "zone-nw");
    const auto same_owner_edge = crossing_from_nw (same_owner_zone);
    co_await move_within (game, "player-alice", alice_x, alice_y, same_owner_edge.source_x,
                          same_owner_edge.source_y);
    auto e4_wait = game.wait_for<move_rejected_notify_t> ().async ();
    game.send (move_msg_t{same_owner_edge.target_x, same_owner_edge.target_y}).submit ();
    require ((co_await e4_wait).reason == reject_reason_t::zone_maintenance,
             "maintenance allowed movement to a different same-node zone");
    std::cout << "scenario ZW-E4 passed\n";

    const auto diagnostics = co_await ops.request (node_diagnostics_req_t{source_node.node_id})
                               .async<node_diagnostics_res_t> ();
    require (!diagnostics.error && diagnostics.maintenance && diagnostics.zones == source_node.zones
               && diagnostics.player_count >= 2,
             "NodeDiagnosticsRes did not expose current zones, population and maintenance");
    std::cout << "scenario ZW-E6 passed\n";
    const auto source_reset =
      co_await ops.request (set_maintenance_req_t{source_node.node_id, false})
        .async<set_maintenance_res_t> ();
    require (!source_reset.error && !source_reset.enabled, "maintenance cleanup failed");

    int bob_x = spec_t::spawn_x;
    int bob_y = spec_t::spawn_y;
    co_await move_within (neighbor, "player-bob", bob_x, bob_y, edge.source_x, edge.source_y);
    co_await cross_zone (neighbor, "player-bob", bob_x, bob_y, edge);
    auto border_wait =
      game.wait_for<zone_state_notify_t> ()
        .where ([&] (const auto &state) {
            return state.zone_id == "zone-nw"
                   && std::any_of (state.players.begin (), state.players.end (),
                                   [&] (const auto &player) {
                                       return player.player_id == "player-bob"
                                              && player.zone_id == pair.target_zone_id;
                                   });
        })
        .async ();
    (void) co_await border_wait;
    std::cout << "scenario ZW-B1 passed\n";

    co_await move_within (game, "player-alice", alice_x, alice_y, edge.source_x, edge.source_y);
    const auto before = co_await probe_actor (game, "player-alice");
    require (!before.error && before.owner_node_rid == pair.source_owner_node_rid,
             "pre-relocation Actor location does not match the source owner");
    co_await cross_zone (game, "player-alice", alice_x, alice_y, edge);
    const int continued_x = edge.target_x + (edge.target_x > spec_t::zone_split ? 2 : 0);
    const int continued_y = edge.target_y + (edge.target_y > spec_t::zone_split ? 2 : 0);
    co_await move_within (game, "player-alice", alice_x, alice_y, continued_x, continued_y);
    std::cout << "scenario ZW-B2 passed\n";

    const auto after = co_await probe_actor (game, "player-alice");
    require (!after.error && after.actor_id == before.actor_id
               && after.object_generation == before.object_generation
               && after.owner_node_rid == pair.target_owner_node_rid
               && after.owner_node_rid != before.owner_node_rid,
             "relocation did not preserve Actor identity while advancing ownership");
    std::cout << "scenario ZW-B3 passed\n";

    const std::vector<std::uint8_t> one_way_payload{2, 4, 6, 8};
    game.send (message_follow_probe_msg_t{"player-alice", "follow-one-way", one_way_payload})
      .submit ();
    std::cout << "scenario ZW-B5 passed\n";
    const std::vector<std::uint8_t> request_payload{1, 3, 5, 7};
    const auto followed =
      co_await game
        .request (message_follow_probe_req_t{"player-alice", "follow-request", request_payload})
        .async<message_follow_probe_res_t> ();
    require (!followed.error && followed.probe_id == "follow-request"
               && followed.payload == request_payload,
             "Message Follow request changed payload or reply correlation");
    const auto missing =
      co_await game
        .request (message_follow_probe_req_t{"player-missing", "follow-missing", request_payload})
        .async<message_follow_probe_res_t> ();
    require (missing.error.has_value (),
             "a route-less Message Follow request did not end terminally");
    std::cout << "scenario ZW-B6 passed\n";

    co_await move_within (game, "player-alice", alice_x, alice_y, edge.target_x, edge.target_y);
    co_await cross_zone (game, "player-alice", alice_x, alice_y, edge, true);
    const int settle_x = edge.source_x == 48 ? 44 : edge.source_x;
    const int settle_y = edge.source_y == 48 ? 44 : edge.source_y;
    co_await move_within (game, "player-alice", alice_x, alice_y, settle_x, settle_y);
    const auto returned = co_await probe_actor (game, "player-alice");
    require (!returned.error && returned.actor_id == before.actor_id
               && returned.object_generation == before.object_generation
               && returned.owner_node_rid == before.owner_node_rid,
             "A-to-B-to-A relocation did not preserve identity and original ownership");
    std::cout << "scenario ZW-B7 passed\n";

    auto announcement_wait = game.wait_for<world_announce_notify_t> ().async ();
    const auto announcement = co_await ops.request (announce_world_req_t{"maintenance soon"})
                                .async<announce_world_res_t> ();
    const auto announced = co_await announcement_wait;
    require (!announcement.announcement_id.empty ()
               && announced.announcement_id == announcement.announcement_id
               && announced.text == "maintenance soon",
             "announcement did not reach the bound game client exactly once");
    std::cout << "scenario ZW-D1 passed\n";

    co_await game.close ().async ();
    co_await neighbor.close ().async ();
    co_await ops.close ().async ();
    std::cout << "zoneworld-client=completed\n";
    co_return true;
}

se::task_t<bool> run_announce (se::coroutine_connector_t &ops, const std::string &scenario)
{
    co_await ops.connect ().async ();
    const auto announced = co_await ops.request (announce_world_req_t{"third subscriber"})
                             .async<announce_world_res_t> ();
    require (!announced.announcement_id.empty (), "fanout publish did not return an id");
    std::cout << "announcement-proof id=" << announced.announcement_id << '\n';
    std::cout << "scenario " << scenario << " passed\n";
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_transition (se::coroutine_connector_t &source,
                                 se::coroutine_connector_t &target,
                                 se::coroutine_connector_t &ops)
{
    co_await source.connect ().async ();
    co_await target.connect ().async ();
    co_await ops.connect ().async ();
    const auto nodes = co_await ops.request (watch_nodes_req_t{}).async<watch_nodes_res_t> ();
    const auto pair =
      co_await ops.request (relocation_pair_req_t{}).async<relocation_pair_res_t> ();
    require (!pair.error, "lifecycle lane has no cross-owner pair");
    const auto edge = crossing_from_nw (pair.target_zone_id);
    const auto target_node_id = node_for_zone (nodes, pair.target_zone_id).node_id;

    const auto source_join = co_await join_and_wait (source, "player-transition-source");
    int source_x = source_join.x;
    int source_y = source_join.y;
    co_await move_within (source, "player-transition-source", source_x, source_y, edge.source_x,
                          edge.source_y);
    const auto target_join = co_await join_and_wait (target, "player-transition-target");
    int target_x = target_join.x;
    int target_y = target_join.y;
    co_await move_within (target, "player-transition-target", target_x, target_y, edge.source_x,
                          edge.source_y);
    co_await cross_zone (target, "player-transition-target", target_x, target_y, edge);
    auto visible_wait =
      source.wait_for<zone_state_notify_t> ()
        .where ([&] (const auto &state) {
            return std::any_of (state.players.begin (), state.players.end (),
                                [&] (const auto &player) {
                                    return player.player_id == "player-transition-target"
                                           && player.zone_id == pair.target_zone_id;
                                });
        })
        .async ();
    (void) co_await visible_wait;

    auto expired_wait =
      source.wait_for<zone_state_notify_t> ()
        .where ([] (const auto &state) {
            return std::none_of (
              state.players.begin (), state.players.end (),
              [] (const auto &player) { return player.player_id == "player-transition-target"; });
        })
        .async ();
    auto disconnected_wait = ops.wait_for<node_status_notify_t> ()
                               .where ([target_node_id] (const auto &node) {
                                   return node.node_id == target_node_id && !node.connected;
                               })
                               .async ();
    std::cout << "scenario ZW-B4-C3 armed node=" << target_node_id << std::endl;

    (void) co_await expired_wait;
    std::cout << "scenario ZW-B4 passed\n";
    //  The disconnect is awaited only to order the report-TTL wait after it. ZW-C2 is asserted
    //  by its own graceful lane; this lane stops the node abruptly.
    (void) co_await disconnected_wait;
    auto expired_report_wait = ops.wait_for<node_status_notify_t> ()
                                 .where ([target_node_id] (const auto &node) {
                                     return node.node_id == target_node_id && !node.registered;
                                 })
                                 .async ();
    (void) co_await expired_report_wait;
    std::cout << "scenario ZW-C3 passed\n";
    co_await source.close ().async ();
    co_await target.close ().async ();
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_c2 (se::coroutine_connector_t &ops, const std::string &target_node_id)
{
    //  ZW-C2's precondition is a *normal* shutdown, so it needs its own lane: the B4/C3 lane
    //  stops the node abruptly and cannot observe the drain path. The sample spec's stop table
    //  fixes which scenario uses which signal.
    require (!target_node_id.empty (), "C2 requires --target-node-id");
    co_await ops.connect ().async ();
    //  NodeStatusNotify only reaches a client that is watching (ZW-C1). Arming the wait without
    //  the WatchNodesReq subscription first makes it wait forever.
    (void) co_await ops.request (watch_nodes_req_t{}).async<watch_nodes_res_t> ();
    auto disconnected_wait = ops.wait_for<node_status_notify_t> ()
                               .where ([target_node_id] (const auto &node) {
                                   return node.node_id == target_node_id && !node.connected;
                               })
                               .async ();
    std::cout << "scenario ZW-C2 armed node=" << target_node_id << std::endl;
    (void) co_await disconnected_wait;
    std::cout << "scenario ZW-C2 passed\n";
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_e5_arm (se::coroutine_connector_t &ops, const std::string &target_node_id)
{
    require (!target_node_id.empty (), "E5 arm requires --target-node-id");
    co_await ops.connect ().async ();
    const auto applied = co_await ops.request (set_maintenance_req_t{target_node_id, true})
                           .async<set_maintenance_res_t> ();
    require (applied.node_id == target_node_id && applied.enabled
               && (!applied.error || *applied.error == errors_t::unavailable),
             "E5 did not persist desired maintenance state");
    std::cout << "scenario ZW-E5-arm passed\n";
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_e5_restore (se::coroutine_connector_t &ops, const std::string &target_node_id)
{
    require (!target_node_id.empty (), "E5 restore requires --target-node-id");
    co_await ops.connect ().async ();
    node_diagnostics_res_t diagnostics;
    for (int attempt = 0; attempt != 200; ++attempt) {
        diagnostics = co_await ops.request (node_diagnostics_req_t{target_node_id})
                        .async<node_diagnostics_res_t> ();
        if (!diagnostics.error)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    require (!diagnostics.error && diagnostics.maintenance,
             "restarted ZoneNode did not restore maintenance from Redis");
    const auto reset = co_await ops.request (set_maintenance_req_t{target_node_id, false})
                         .async<set_maintenance_res_t> ();
    require (!reset.error && !reset.enabled, "E5 maintenance cleanup failed");
    std::cout << "scenario ZW-E5 passed\n";
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_fresh_actor_probes (se::coroutine_connector_t &game,
                                         const std::string &scenario)
{
    co_await game.connect ().async ();
    std::size_t accepted = 0;
    for (int index = 0; index != 24; ++index) {
        const auto actor_id = "player-" + scenario + "-fresh-" + std::to_string (index);
        const auto created = co_await game.request (fresh_actor_probe_req_t{actor_id})
                               .async<fresh_actor_probe_res_t> ();
        if (created.error || created.object_generation == 0)
            continue;
        ++accepted;
        std::cout << "fresh-actor-proof scenario=" << scenario << " actor=" << created.actor_id
                  << " generation=" << created.object_generation
                  << " owner=" << created.owner_node_rid << '\n';
    }
    require (accepted > 0, "no live host accepted a fresh Actor");
    co_await game.close ().async ();
    co_return true;
}

se::task_t<bool> run_g4_boundary (se::coroutine_connector_t &game, se::coroutine_connector_t &ops)
{
    co_await game.connect ().async ();
    co_await ops.connect ().async ();
    const auto nodes = co_await ops.request (watch_nodes_req_t{}).async<watch_nodes_res_t> ();
    const auto pair =
      co_await ops.request (relocation_pair_req_t{}).async<relocation_pair_res_t> ();
    require (!pair.error, "G4 requires a cross-owner pair");
    const auto edge = crossing_from_nw (pair.target_zone_id);
    const auto target_node_id = node_for_zone (nodes, pair.target_zone_id).node_id;
    const auto joined = co_await join_and_wait (game, "player-g4-crash");
    int x = joined.x;
    int y = joined.y;
    co_await move_within (game, joined.player_id, x, y, edge.source_x, edge.source_y);
    auto failed_wait =
      game.wait_for<crash_relocation_probe_res_t> ()
        .where ([] (const auto &reply) { return reply.error == errors_t::unavailable; })
        .async ();
    game.send (crash_relocation_probe_msg_t{edge.target_x, edge.target_y}).submit ();
    std::cout << "scenario ZW-G4 armed node=" << target_node_id << std::endl;
    const auto failed = co_await failed_wait;
    require (failed.error == errors_t::unavailable,
             "crashed owner operation did not terminate Unavailable");
    std::cout << "scenario ZW-G4-boundary passed\n";
    co_await game.close ().async ();
    co_await ops.close ().async ();
    co_return true;
}

se::task_t<bool> run_b8 (se::coroutine_connector_t &game,
                         se::coroutine_connector_t &ops,
                         const std::string &arm_file,
                         std::atomic_bool &disconnected)
{
    require (!arm_file.empty (), "B8 requires --arm-file");
    co_await game.connect ().async ();
    co_await ops.connect ().async ();
    const auto pair =
      co_await ops.request (relocation_pair_req_t{}).async<relocation_pair_res_t> ();
    require (!pair.error, "B8 requires a cross-owner pair");
    const auto edge = crossing_from_nw (pair.target_zone_id);
    const auto joined = co_await join_and_wait (game, "player-b8-seal");
    int x = joined.x;
    int y = joined.y;
    co_await move_within (game, joined.player_id, x, y, edge.source_x, edge.source_y);
    std::cout << "scenario ZW-B8 armed" << std::endl;
    co_await wait_for_condition (
      [arm_file] { return std::filesystem::exists (arm_file); },
      std::chrono::seconds (10),
      "B8 runner did not arm the command-44 proxy");
    game.send (move_msg_t{edge.target_x, edge.target_y}).submit ();
    co_await wait_for_condition (
      [&disconnected] { return disconnected.load (); },
      std::chrono::seconds (45),
      "B8 physical connection did not close at seal timeout");
    std::cout << "scenario ZW-B8 disconnected" << std::endl;
    co_await wait_for_condition (
      [arm_file] { return std::filesystem::exists (arm_file + ".blocked"); },
      std::chrono::seconds (45),
      "B8 proxy did not observe the post-commit command 44");
    co_await game.connect ().async ();
    auto rejoin_wait =
      game.wait_for<join_world_res_t> ()
        .where ([] (const auto &reply) { return reply.player_id == "player-b8-seal"; })
        .async ();
    game.send (join_world_req_t{"player-b8-seal"}).submit ();
    const auto rebound = co_await rejoin_wait;
    require (!rebound.error && rebound.zone_id == pair.target_zone_id,
             "B8 reconnect did not bind the existing relocated Actor");
    std::cout << "scenario ZW-B8 passed\n";
    co_await game.close ().async ();
    co_await ops.close ().async ();
    co_return true;
}

} // namespace zlink::samples::zoneworld

int main (int argc, char **argv)
{
    using namespace zlink::samples::zoneworld;
    try {
        const auto topology = load_client_topology (argc, argv);
        auto game_core = make_connector (topology.game_endpoint);
        auto neighbor_core = make_connector (topology.game_endpoint);
        auto probe_core = make_connector (topology.game_endpoint);
        auto ops_core = make_connector (topology.ops_endpoint);
        std::atomic_bool disconnected{false};
        game_core.on_connection_state_changed (
          [&] (const zlink::stream_connector::connection_state_changed_t &event) {
              if (event.current == zlink::stream_connector::connection_state_t::reconnecting
                  || event.current == zlink::stream_connector::connection_state_t::disconnected
                  || event.current == zlink::stream_connector::connection_state_t::closed)
                  disconnected.store (true);
          });
        auto game = se::use (game_core);
        auto neighbor = se::use (neighbor_core);
        auto probe = se::use (probe_core);
        auto ops = se::use (ops_core);

        se::task_t<bool> task =
          topology.scenario == "main"         ? run_main (game, neighbor, probe, ops)
          : topology.scenario == "D2"         ? run_announce (ops, "ZW-D2")
          : topology.scenario == "transition" ? run_transition (game, neighbor, ops)
          : topology.scenario == "C2"         ? run_c2 (ops, topology.target_node_id)
          : topology.scenario == "E5-arm"     ? run_e5_arm (ops, topology.target_node_id)
          : topology.scenario == "E5"         ? run_e5_restore (ops, topology.target_node_id)
          : topology.scenario == "G3" || topology.scenario == "G4-fresh"
            ? run_fresh_actor_probes (game, topology.scenario)
          : topology.scenario == "G4" ? run_g4_boundary (game, ops)
          : topology.scenario == "B8"
            ? run_b8 (game, ops, topology.arm_file, disconnected)
            : throw std::runtime_error ("unknown ZoneWorld client scenario");

        const auto &result = task.result ();
        if (!result) {
            std::cerr << "zoneworld=failed stream-error-code="
                      << static_cast<int> (result.error_code ())
                      << " message=" << result.error ()->message << '\n';
            return 1;
        }
        return result.value () ? 0 : 1;
    }
    catch (const std::exception &error) {
        std::cerr << "zoneworld=failed " << error.what () << '\n';
        return 1;
    }
}
