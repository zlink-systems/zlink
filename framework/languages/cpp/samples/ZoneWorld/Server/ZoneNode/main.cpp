/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

struct node_state_t
{
    std::string node_id;
    std::atomic_bool maintenance{false};
    std::mutex residents_mutex;
    std::set<std::string> human_residents;
};

node_state_t *g_node_state = nullptr;

class player_actor_t final : public fw::actor_t
{
  public:
    explicit player_actor_t (fw::actor_context_t context) : _context (std::move (context))
    {
        player_id = std::string (_context.actor_ref ().actor_id ().value ());
    }

    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        if (std::holds_alternative<fw::actor_join_accepted_t> (completion)) {
            std::cerr << "zoneworld-join-accepted player=" << player_id
                      << " zone=" << zone_id << '\n';
            initial_entry = false;
        } else if (const auto *failed = std::get_if<fw::actor_join_failed_t> (&completion)) {
            std::cerr << "zoneworld-join-failed player=" << player_id
                      << " kind=" << static_cast<int> (failed->error_kind) << '\n';
        } else if (const auto *rejected = std::get_if<fw::actor_join_rejected_t> (&completion)) {
            std::cerr << "zoneworld-join-rejected player=" << player_id << '\n';
            auto reason = std::string (reject_reason_t::zone_maintenance);
            if (rejected->reply) {
                reason = rejected->reply->decode<enter_zone_res_t> ().error.value_or (reason);
            }
            if (!is_bot) {
                _context.bound_session ().send (move_rejected_notify_t{reason, x, y}).submit ();
            }
        }
        co_return;
    }

    std::string player_id;
    int x = 25;
    int y = 25;
    std::string zone_id = "zone-nw";
    bool is_bot = false;
    int dir_x = 0;
    int dir_y = 0;
    bool initial_entry = true;

  private:
    fw::actor_context_t _context;
};

class player_actor_factory_t final : public fw::actor_factory_t<player_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<player_actor_t>> create (fw::actor_context_t context,
                                                        std::stop_token) override
    {
        co_return std::make_shared<player_actor_t> (std::move (context));
    }
};

struct player_state_t { int x = 25; int y = 25; std::string zone_id = "zone-nw"; bool is_bot = false; int dir_x = 0; int dir_y = 0; bool initial_entry = true; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (player_state_t, x, y, zone_id, is_bot, dir_x, dir_y, initial_entry)

class player_relocation_adapter_t final : public fw::actor_relocation_adapter_t<player_actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (player_actor_t &actor, std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (
          player_state_t{actor.x, actor.y, actor.zone_id, actor.is_bot, actor.dir_x, actor.dir_y,
                         actor.initial_entry});
        co_return std::vector<std::byte> (message.bytes ().begin (), message.bytes ().end ());
    }

    fw::task_t<void> restore (player_actor_t &actor, std::vector<std::byte> payload,
                              std::stop_token) override
    {
        const auto restored = zlink::message_t::from (
          std::span<const std::byte> (payload.data (), payload.size ())).parse_json<player_state_t> ();
        actor.x = restored.x; actor.y = restored.y; actor.zone_id = restored.zone_id;
        actor.is_bot = restored.is_bot; actor.dir_x = restored.dir_x; actor.dir_y = restored.dir_y;
        actor.initial_entry = restored.initial_entry;
        co_return;
    }
};

class zone_entry_spot_t final : public fw::entry_spot_t<player_actor_t>
{
  public:
    explicit zone_entry_spot_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_actor_request<&zone_entry_spot_t::join_world> (join_world_req_t::packet_name)
          .add_actor_request<&zone_entry_spot_t::enter_world> (enter_world_req_t::packet_name)
          .add_actor_send<&zone_entry_spot_t::move> (move_msg_t::packet_name)
          .add_actor_request<&zone_entry_spot_t::follow_probe> (message_follow_probe_req_t::packet_name);
    }

    fw::task_t<fw::actor_create_response_t> on_create_actor (player_actor_t &, const fw::message_t &) override
    { co_return fw::actor_create_response_t::accept (); }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view, const fw::message_t &) override
    { co_return fw::spot_actor_join_result_t::accept (); }
    fw::task_t<void> on_actor_joined (player_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (player_actor_t &) override { co_return; }

    join_world_res_t join_world (player_actor_t &actor, fw::message_context_t &,
                                 const join_world_req_t &request)
    {
        actor.player_id = request.player_id;
        actor.x = 25; actor.y = 25; actor.zone_id = "zone-nw"; actor.is_bot = false;
        actor.initial_entry = true;
        actor.context ().join_spot (actor.zone_id,
          enter_zone_msg_t{actor.player_id, actor.x, actor.y, false, true, std::nullopt}).defer ();
        return {actor.player_id, actor.zone_id, actor.x, actor.y, std::nullopt};
    }

    enter_world_res_t enter_world (player_actor_t &actor, fw::message_context_t &,
                                   const enter_world_req_t &request)
    {
        actor.x = request.x; actor.y = request.y; actor.zone_id = zone_of (request.x, request.y);
        actor.is_bot = request.is_bot; actor.dir_x = request.dir_x; actor.dir_y = request.dir_y;
        actor.initial_entry = true;
        std::cerr << "zoneworld-enter-world player=" << actor.player_id
                  << " zone=" << actor.zone_id << '\n';
        actor.context ().join_spot (actor.zone_id,
          enter_zone_msg_t{actor.player_id, actor.x, actor.y, actor.is_bot, true, std::nullopt})
          .defer ();
        return {actor.zone_id, actor.x, actor.y, std::nullopt};
    }

    void move (player_actor_t &actor, fw::message_context_t &, const move_msg_t &message)
    {
        std::cerr << "zoneworld-entry-move player=" << actor.player_id << " x=" << message.x << '\n';
        apply_move (actor, message);
    }

    message_follow_probe_res_t follow_probe (player_actor_t &, fw::message_context_t &,
                                             const message_follow_probe_req_t &request)
    { return {request.probe_id, request.payload}; }

    static void apply_move (player_actor_t &actor, const move_msg_t &message)
    {
        if (const auto error = validate_move (actor.x, actor.y, message.x, message.y, false)) {
            if (actor.is_bot) {
                actor.dir_x = -actor.dir_x;
                actor.dir_y = -actor.dir_y;
                return;
            }
            actor.context ().bound_session ()
              .send (move_rejected_notify_t{*error, actor.x, actor.y}).submit ();
            return;
        }
        const auto target = zone_of (message.x, message.y);
        if (target != actor.zone_id) {
            actor.x = message.x; actor.y = message.y; actor.zone_id = target;
            actor.initial_entry = false;
            actor.context ().join_spot (target,
              enter_zone_msg_t{actor.player_id, message.x, message.y, actor.is_bot, false,
                               g_node_state->node_id})
              .timeout (std::chrono::seconds (15)).defer ();
            return;
        }
        actor.x = message.x; actor.y = message.y;
        actor.context ().bound_session ().send (
          zone_state_notify_t{actor.zone_id, 0,
            {{actor.player_id, actor.x, actor.y, actor.zone_id, actor.is_bot}}}).submit ();
    }

  private:
    fw::entry_spot_context_t _context;
};

struct zone_tick_handler_t;
struct bot_tick_handler_t;

class zone_spot_t final : public fw::spot_t<player_actor_t>
{
  public:
    zone_spot_t (fw::spot_context_t context, fw::actor_client_t &actors) :
        _context (std::move (context)), _actor_client (actors)
    {
    }
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_actor_send<&zone_spot_t::move> (move_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::bot_tick> (bot_tick_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_state> (deliver_zone_state_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_changed> (deliver_zone_changed_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_announce> (deliver_world_announce_msg_t::packet_name)
          .add_handler<&zone_spot_t::announce> (deliver_announce_msg_t::packet_name)
          .add_actor_request<&zone_spot_t::follow_probe> (message_follow_probe_req_t::packet_name);
        for (const auto &from : adjacent_zones (_context.spot_id ()))
            _context.handlers ().add_subscribe<&zone_spot_t::border> (border_topic (from, _context.spot_id ()));
    }

    fw::task_t<void> on_initialize () override
    {
        _timer = _context.add_timer<zone_tick_handler_t> ("zone-tick", std::chrono::milliseconds (100));
        _bot_timer = _context.add_timer<bot_tick_handler_t> (
          "bot-tick", std::chrono::milliseconds (500));
        std::cerr << "zoneworld-zone-ready node=" << g_node_state->node_id
                  << " zone=" << _context.spot_id () << '\n';
        co_return;
    }
    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &) override
    { co_return fw::spot_create_response_t::accept (); }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        const auto enter = request.decode<enter_zone_msg_t> ();
        if (g_node_state->maintenance.load ()
            && (!enter.from_node_id || *enter.from_node_id != g_node_state->node_id))
            co_return fw::spot_actor_join_result_t::reject (
              enter_zone_res_t{_context.spot_id (), reject_reason_t::zone_maintenance});
        std::lock_guard lock (_mutex);
        _pending[std::string (actor_id)] = enter;
        co_return fw::spot_actor_join_result_t::accept (
          enter_zone_res_t{_context.spot_id (), std::nullopt});
    }
    fw::task_t<void> on_actor_joined (player_actor_t &actor) override
    {
        std::cerr << "zoneworld-actor-joined zone=" << _context.spot_id ()
                  << " player=" << actor.player_id << '\n';
        enter_zone_msg_t enter;
        {
            std::lock_guard lock (_mutex);
            const auto found = _pending.find (actor.player_id);
            if (found == _pending.end ()) co_return;
            enter = found->second; _pending.erase (found);
            _players[actor.player_id] = {actor.player_id, enter.x, enter.y, _context.spot_id (), enter.is_bot};
        }
        actor.x = enter.x; actor.y = enter.y; actor.zone_id = _context.spot_id (); actor.is_bot = enter.is_bot;
        if (!actor.is_bot && !enter.initial_entry)
            co_await _actor_client
              .send (fw::actor_id_t (actor.player_id),
                     deliver_zone_changed_msg_t{actor.player_id, actor.zone_id})
              .submit ();
        if (!actor.is_bot) {
            std::lock_guard residents_lock (g_node_state->residents_mutex);
            g_node_state->human_residents.insert (actor.player_id);
        }
        co_return;
    }
    fw::task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        {
            std::lock_guard lock (_mutex);
            _players.erase (actor.player_id);
        }
        if (!actor.is_bot) {
            std::lock_guard residents_lock (g_node_state->residents_mutex);
            g_node_state->human_residents.erase (actor.player_id);
        }
        co_return;
    }

    void move (player_actor_t &actor, fw::message_context_t &, const move_msg_t &message)
    {
        std::cerr << "zoneworld-zone-move zone=" << _context.spot_id ()
                  << " player=" << actor.player_id << " x=" << message.x << '\n';
        zone_entry_spot_t::apply_move (actor, message);
        if (actor.zone_id == _context.spot_id ()) {
            std::lock_guard lock (_mutex);
            _players[actor.player_id] = {actor.player_id, actor.x, actor.y, actor.zone_id, actor.is_bot};
        }
    }
    void bot_tick (player_actor_t &actor, fw::message_context_t &, const bot_tick_msg_t &)
    {
        if (!actor.is_bot) return;
        std::cerr << "zoneworld-bot-move player=" << actor.player_id
                  << " from=" << actor.x << ',' << actor.y << '\n';
        zone_entry_spot_t::apply_move (
          actor, move_msg_t{actor.x + actor.dir_x * 3, actor.y + actor.dir_y * 3});
        if (actor.zone_id == _context.spot_id ()) {
            std::lock_guard lock (_mutex);
            _players[actor.player_id] =
              {actor.player_id, actor.x, actor.y, actor.zone_id, true};
        }
    }
    message_follow_probe_res_t follow_probe (player_actor_t &, fw::message_context_t &,
                                             const message_follow_probe_req_t &request)
    { return {request.probe_id, request.payload}; }
    void deliver_state (player_actor_t &actor, fw::message_context_t &,
                        const deliver_zone_state_msg_t &message)
    {
        if (!actor.is_bot)
            actor.context ().bound_session ().send (
              zone_state_notify_t{message.zone_id, message.tick, message.players}).submit ();
    }
    void deliver_changed (player_actor_t &actor, fw::message_context_t &,
                          const deliver_zone_changed_msg_t &message)
    {
        if (!actor.is_bot)
            actor.context ().bound_session ().send (
              zone_changed_notify_t{message.player_id, message.zone_id}).submit ();
    }
    void deliver_announce (player_actor_t &actor, fw::message_context_t &,
                           const deliver_world_announce_msg_t &message)
    {
        if (!actor.is_bot) {
            std::cerr << "zoneworld-announce-deliver node=" << g_node_state->node_id
                      << " player=" << actor.player_id
                      << " id=" << message.announcement_id << '\n';
            actor.context ().bound_session ().send (
              world_announce_notify_t{message.announcement_id, message.text}).submit ();
        }
    }
    fw::task_t<void> announce (const deliver_announce_msg_t &message)
    {
        std::cerr << "zoneworld-announce-zone zone=" << _context.spot_id ()
                  << " id=" << message.announcement_id << '\n';
        std::vector<std::string> human_ids;
        {
            std::lock_guard lock (_mutex);
            for (const auto &[player_id, player] : _players)
                if (!player.is_bot) human_ids.push_back (player_id);
        }
        for (const auto &player_id : human_ids)
            co_await _actor_client
              .send (fw::actor_id_t (player_id),
                     deliver_world_announce_msg_t{message.announcement_id, message.text})
              .submit ();
    }
    void border (const zone_border_event_t &event)
    {
        if (!event.players.empty ())
            std::cerr << "zoneworld-border-received zone=" << _context.spot_id ()
                      << " from=" << event.from_zone_id << " tick=" << event.tick << '\n';
        std::lock_guard lock (_mutex);
        auto &current = _borders[event.from_zone_id];
        if (event.tick > current.tick) current = event;
    }
    fw::task_t<void> tick ()
    {
        std::vector<player_view_t> local;
        std::vector<std::string> human_ids;
        {
            std::lock_guard lock (_mutex);
            ++_tick;
            for (const auto &[_, player] : _players) local.push_back (player);
            for (const auto &[player_id, player] : _players)
                if (!player.is_bot) human_ids.push_back (player_id);
        }
        sort_players (local);
        for (const auto &to : adjacent_zones (_context.spot_id ())) {
            std::vector<player_view_t> border_players;
            std::copy_if (local.begin (), local.end (), std::back_inserter (border_players),
              [&] (const auto &p) { return std::abs (p.x - 50) <= 10 || std::abs (p.y - 50) <= 10; });
            _context.publish (border_topic (_context.spot_id (), to),
                              zone_border_event_t{_context.spot_id (), to, _tick, border_players}).submit ();
        }
        std::map<std::string, player_view_t> merged;
        {
            std::lock_guard lock (_mutex);
            for (auto it = _borders.begin (); it != _borders.end ();) {
                if (_tick - it->second.tick > 3) it = _borders.erase (it);
                else { for (const auto &player : it->second.players) merged[player.player_id] = player; ++it; }
            }
        }
        for (const auto &player : local) merged[player.player_id] = player;
        std::vector<player_view_t> snapshot;
        for (const auto &[_, player] : merged) snapshot.push_back (player);
        sort_players (snapshot);
        for (const auto &player_id : human_ids) {
            try {
                co_await _actor_client
                  .send (fw::actor_id_t (player_id),
                         deliver_zone_state_msg_t{_context.spot_id (), _tick, snapshot})
                  .submit ();
            }
            catch (const fw::framework_exception_t &) {
                // A stale or disconnected player must not terminate the periodic zone tick.
            }
        }
        co_return;
    }
    fw::task_t<void> tick_bots ()
    {
        std::vector<std::string> bot_ids;
        {
            std::lock_guard lock (_mutex);
            for (const auto &[player_id, player] : _players)
                if (player.is_bot) bot_ids.push_back (player_id);
        }
        for (const auto &player_id : bot_ids)
            co_await _actor_client.send (fw::actor_id_t (player_id), bot_tick_msg_t{}).submit ();
    }

  private:
    fw::spot_context_t _context;
    fw::actor_client_t &_actor_client;
    fw::timer_t _timer;
    fw::timer_t _bot_timer;
    std::mutex _mutex;
    std::map<std::string, enter_zone_msg_t> _pending;
    std::map<std::string, player_view_t> _players;
    std::map<std::string, zone_border_event_t> _borders;
    std::int64_t _tick = 0;
};

struct zone_tick_handler_t
{
    fw::task_t<void> handle (zone_spot_t &spot, const fw::timer_tick_t &) const
    { return spot.tick (); }
};
struct bot_tick_handler_t
{
    fw::task_t<void> handle (zone_spot_t &spot, const fw::timer_tick_t &) const
    { return spot.tick_bots (); }
};

struct maintenance_handler_t
{
    using event_type = node_maintenance_changed_event_t;
    static constexpr const char *topic_name = names_t::maintenance_topic;
    void handle (const node_maintenance_changed_event_t &event,
                 const fw::publish_message_context_t &)
    {
        if (event.node_id == g_node_state->node_id) {
            g_node_state->maintenance.store (event.enabled);
            std::cout << "zoneworld-maintenance node=" << event.node_id
                      << " enabled=" << event.enabled << '\n';
        }
    }
};

class announce_handler_t
{
  public:
    using event_type = world_announce_event_t;
    using dependency_types = fw::dependency_list_t<fw::actor_client_t>;
    static constexpr const char *topic_name = names_t::announce_topic;

    explicit announce_handler_t (fw::actor_client_t &actors) : _actors (actors) {}

    fw::task_t<void> handle (const world_announce_event_t &event,
                             const fw::publish_message_context_t &)
    {
        std::cerr << "zoneworld-announce-received node=" << g_node_state->node_id
                  << " id=" << event.announcement_id << '\n';
        std::vector<std::string> player_ids;
        {
            std::lock_guard lock (g_node_state->residents_mutex);
            player_ids.assign (g_node_state->human_residents.begin (),
                               g_node_state->human_residents.end ());
        }
        for (const auto &player_id : player_ids) {
            try {
                co_await _actors
                  .send (fw::actor_id_t (player_id),
                         deliver_world_announce_msg_t{event.announcement_id, event.text})
                  .submit ();
            }
            catch (const fw::framework_exception_t &error) {
                std::cerr << "zoneworld-announce-skip node=" << g_node_state->node_id
                          << " player=" << player_id << " error=" << error.what () << '\n';
            }
        }
        co_return;
    }

  private:
    fw::actor_client_t &_actors;
};

struct bootstrap_req_t { static constexpr const char *packet_name = "ZoneWorldBootstrapReq"; };
struct bootstrap_res_t { static constexpr const char *packet_name = "ZoneWorldBootstrapRes"; int zones = 0; };
inline void to_json (nlohmann::json &j, const bootstrap_req_t &) { j = nlohmann::json::object (); }
inline void from_json (const nlohmann::json &, bootstrap_req_t &) {}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (bootstrap_res_t, zones)

class bootstrap_handler_t
{
  public:
    using request_type = bootstrap_req_t;
    using reply_type = bootstrap_res_t;
    using dependency_types = fw::dependency_list_t<fw::spot_manager_t>;
    static constexpr const char *topic_name = "ZoneWorldBootstrap";

    explicit bootstrap_handler_t (fw::spot_manager_t &spots) : _spots (spots) {}

    fw::task_t<bootstrap_res_t> handle (const bootstrap_req_t &)
    {
        const auto zones = g_node_state->node_id == "zone-node-1"
          ? std::array<const char *, 2>{"zone-nw", "zone-sw"}
          : std::array<const char *, 2>{"zone-ne", "zone-se"};
        for (const auto *zone : zones) {
            std::cerr << "zoneworld-bootstrap-create node=" << g_node_state->node_id
                      << " zone=" << zone << '\n';
            (void) co_await _spots.get_or_create (zone, names_t::zone_spot)
              .in_mesh (names_t::mesh).submit ();
            std::cerr << "zoneworld-bootstrap-created node=" << g_node_state->node_id
                      << " zone=" << zone << '\n';
        }
        co_return bootstrap_res_t{static_cast<int> (zones.size ())};
    }

  private:
    fw::spot_manager_t &_spots;
};

} // namespace zlink::samples::zoneworld

int main (int argc, char **argv)
{
    using namespace zlink::samples::zoneworld;
    namespace fw = zlink::framework;
    configuration_t configuration;
    node_state_t state{configuration.node_id};
    g_node_state = &state;
    auto app = fw::app_t::create ();
    app.logging ().use_console ().set_min_level (fw::log_level_t::info);
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        add_stores (options, configuration);
        auto mesh = options.add_route_mesh (names_t::mesh);
        mesh.set_automatic_routing_id_prefix ("zn").set_object_role (fw::object_role_t::server);
        mesh.channel_name (names_t::zone_channel).server ();
        auto spot_services = options.services ().build_provider ();
        mesh.listen (configuration.mesh_endpoint)
          .add_entry_spot<zone_entry_spot_t> ([] (fw::entry_spot_context_t context) {
              return std::make_shared<zone_entry_spot_t> (std::move (context));
          })
          .add_spot_factory<zone_spot_t> (names_t::zone_spot,
            [spot_services] (fw::spot_context_t context) mutable {
                return std::make_shared<zone_spot_t> (
                  std::move (context), spot_services.get_required<fw::actor_client_t> ());
            },
            [] (auto &factory) { factory.set_stable_type_limit (2).disable_relocation (); })
          .add_actor_factory<player_actor_t, player_actor_factory_t> (
            names_t::player_actor, std::make_shared<player_actor_factory_t> (),
            [] (auto &factory) { factory.template preserve_state_with<player_relocation_adapter_t> (); });
        if (!configuration.peer_endpoint.empty ())
            mesh.peer_connections ().connect (configuration.peer_endpoint);
        if (!configuration.peer_endpoint_2.empty ())
            mesh.peer_connections ().connect (configuration.peer_endpoint_2);
        options.handlers ().group ("zoneworld-broadcast")
          .add_publish<maintenance_handler_t> ()
          .add_publish<announce_handler_t> ();
        options.add_fanout_channel (names_t::broadcast_channel)
          .enable_subscriber ()
          .use_handler_group ("zoneworld-broadcast");
        options.http ().listen (configuration.bootstrap_http_endpoint)
          .map_health ("/health")
          .map_post<bootstrap_handler_t> ("/bootstrap");
    });
    std::cout << "zoneworld-role-ready role=zone-node node=" << configuration.node_id << '\n';
    return app.run (argc, argv);
}
