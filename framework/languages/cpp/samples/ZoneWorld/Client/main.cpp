/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Server/Configuration/configuration.hpp"
#include "../Shared/world_rules.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace zlink::samples::zoneworld
{
namespace se = zlink::stream_e2e_client;

inline void require (bool condition, const char *message)
{
    if (!condition) throw std::runtime_error (message);
}

zlink::stream_connector::connector_t make_connector (const std::string &endpoint)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = endpoint;
    options.connect_timeout = std::chrono::seconds (15);
    options.request_timeout = std::chrono::seconds (30);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    connector.codecs ().enable_codec (zlink::stream_connector::codec_t::json)
      .use_default_codec (zlink::stream_connector::codec_t::json);
    return connector;
}

se::task_t<bool> run (se::coroutine_connector_t &game, se::coroutine_connector_t &neighbor,
                      se::coroutine_connector_t &ops)
{
    std::cerr << "zoneworld-step=connect-game\n";
    co_await game.connect ().async ();
    std::cerr << "zoneworld-step=connect-neighbor\n";
    co_await neighbor.connect ().async ();
    std::cerr << "zoneworld-step=connect-ops\n";
    co_await ops.connect ().async ();

    std::cerr << "zoneworld-step=watch-nodes\n";
    const auto nodes = co_await ops.request (watch_nodes_req_t{}).async<watch_nodes_res_t> ();
    require (nodes.nodes.size () >= 2, "Ops must report two ZoneNodes");
    require (std::all_of (nodes.nodes.begin (), nodes.nodes.end (),
                          [] (const auto &n) { return n.registered && n.connected; }),
             "ZoneNodes must be registered and connected");

    std::cerr << "zoneworld-step=join-alice\n";
    auto first_state_wait = game.wait_for<zone_state_notify_t> ().async ();
    const auto joined = co_await game.request (join_world_req_t{"player-alice"}).async<join_world_res_t> ();
    std::cerr << "zoneworld-step=join-alice-replied zone=" << joined.zone_id
              << " error=" << joined.error.value_or ("none") << '\n';
    require (!joined.error && joined.zone_id == "zone-nw" && joined.x == 25 && joined.y == 25,
             "JoinWorldRes must return the canonical spawn");
    const auto first_state = co_await first_state_wait;
    std::cerr << "zoneworld-step=join-alice-state zone=" << first_state.zone_id << '\n';
    require (first_state.zone_id == "zone-nw", "initial ZoneStateNotify must be zone-nw");

    std::cerr << "zoneworld-step=join-bob\n";
    auto neighbor_initial = neighbor.wait_for<zone_state_notify_t> ().async ();
    (void) co_await neighbor.request (join_world_req_t{"player-bob"}).async<join_world_res_t> ();
    const auto shared_state = co_await neighbor_initial;
    std::cerr << "zoneworld-step=join-bob-state\n";
    if (shared_state.players.size () > 1)
        require (std::is_sorted (shared_state.players.begin (), shared_state.players.end (),
                  [] (const auto &left, const auto &right) {
                      return left.player_id < right.player_id;
                  }), "ZoneStateNotify players are not sorted by PlayerId UTF-8 bytes");
    auto same_zone_wait = game.wait_for<zone_state_notify_t> ()
      .where ([] (const auto &n) {
          return std::any_of (n.players.begin (), n.players.end (), [] (const auto &player) {
              return player.player_id == "player-alice" && player.x == 28 && player.y == 27;
          });
      }).async ();
    game.send (move_msg_t{28, 27}).submit ();
    const auto same_zone = co_await same_zone_wait;
    require (std::any_of (same_zone.players.begin (), same_zone.players.end (), [] (const auto &player) {
                return player.player_id == "player-alice" && player.zone_id == "zone-nw";
             }), "same-zone move changed zone");

    auto rejected_wait = game.wait_for<move_rejected_notify_t> ().async ();
    game.send (move_msg_t{-1, 27}).submit ();
    const auto rejected = co_await rejected_wait;
    require (rejected.reason == reject_reason_t::out_of_range,
             "OutOfRange must be the first rejection");

    rejected_wait = game.wait_for<move_rejected_notify_t> ().async ();
    game.send (move_msg_t{34, 27}).submit ();
    require ((co_await rejected_wait).reason == reject_reason_t::too_far,
             "TooFar must follow the range check");

    for (int x = 33; x <= 48; x += 5) {
        auto state_wait = game.wait_for<zone_state_notify_t> ()
          .where ([x] (const auto &n) {
              return std::any_of (n.players.begin (), n.players.end (), [x] (const auto &player) {
                  return player.player_id == "player-alice" && player.x == x;
              });
          }).async ();
        game.send (move_msg_t{x, 27}).submit ();
        (void) co_await state_wait;
    }
    auto changed_wait = game.wait_for<zone_changed_notify_t> ()
      .where ([] (const auto &n) { return n.zone_id == "zone-ne"; }).async ();
    game.send (move_msg_t{52, 27}).submit ();
    const auto changed = co_await changed_wait;
    require (changed.player_id == "player-alice", "relocation changed ActorId");

    const std::vector<std::uint8_t> payload{1, 3, 5, 7};
    const auto probe = co_await game.request (
      message_follow_probe_req_t{"player-alice", "follow-1", payload})
      .async<message_follow_probe_res_t> ();
    require (probe.probe_id == "follow-1" && probe.payload == payload,
             "Message Follow probe changed payload or reply route");

    auto announcement_wait = game.wait_for<world_announce_notify_t> ().async ();
    const auto announcement = co_await ops.request (announce_world_req_t{"maintenance soon"})
      .async<announce_world_res_t> ();
    require (!announcement.announcement_id.empty (), "announcement id is empty");
    const auto announced = co_await announcement_wait;
    require (announced.announcement_id == announcement.announcement_id
               && announced.text == "maintenance soon",
             "announcement did not reach the bound game client");
    const auto maintenance = co_await ops.request (set_maintenance_req_t{"zone-node-2", true})
      .async<set_maintenance_res_t> ();
    require (!maintenance.error && maintenance.enabled, "targeted maintenance was not applied");
    const auto diagnostics = co_await ops.request (node_diagnostics_req_t{"zone-node-2"})
      .async<node_diagnostics_res_t> ();
    require (!diagnostics.error && diagnostics.maintenance,
             "diagnostics did not reflect maintenance desired state");
    (void) co_await ops.request (set_maintenance_req_t{"zone-node-2", false})
      .async<set_maintenance_res_t> ();

    co_await game.close ().async ();
    co_await neighbor.close ().async ();
    co_await ops.close ().async ();
    std::cout << "zoneworld-relocation=completed\n"
              << "zoneworld-border=completed\n"
              << "zoneworld-ops=completed\n"
              << "zoneworld=completed\n";
    co_return true;
}

} // namespace zlink::samples::zoneworld

int main ()
{
    using namespace zlink::samples::zoneworld;
    try {
        auto game_core = make_connector (env ("ZONEWORLD_GAME_ENDPOINT", "tcp://127.0.0.1:35201"));
        auto neighbor_core = make_connector (env ("ZONEWORLD_GAME_ENDPOINT", "tcp://127.0.0.1:35201"));
        auto ops_core = make_connector (env ("ZONEWORLD_OPS_ENDPOINT", "tcp://127.0.0.1:35202"));
        auto game = zlink::stream_e2e_client::use (game_core);
        auto neighbor = zlink::stream_e2e_client::use (neighbor_core);
        auto ops = zlink::stream_e2e_client::use (ops_core);
        const auto result = run (game, neighbor, ops).result ();
        if (!result) {
            std::cerr << "zoneworld=failed stream-error-code="
                      << static_cast<int> (result.error_code ()) << '\n';
            return 1;
        }
        return result.value () ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "zoneworld=failed " << error.what () << '\n';
        return 1;
    }
}
