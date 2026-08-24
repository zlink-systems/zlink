/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/maintenance_store.hpp"
#include "player_actor_relocation_adapter.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

struct node_state_t
{
    std::string node_id;
    std::optional<std::string> fault_tick_zone;
    std::atomic_bool maintenance{false};
    std::atomic_bool fault_injected{false};
    mutable std::mutex mutex;
    std::set<std::string> zones;
    std::set<std::string> residents;
    std::deque<fw::spot_event_t> spot_events;

    std::vector<std::string> zone_snapshot () const
    {
        std::lock_guard lock (mutex);
        return {zones.begin (), zones.end ()};
    }
    int player_count () const
    {
        std::lock_guard lock (mutex);
        return static_cast<int> (residents.size ());
    }
    void record_spot_event (const fw::spot_event_t &event)
    {
        std::lock_guard lock (mutex);
        if (spot_events.size () == 64)
            spot_events.pop_front ();
        spot_events.push_back (event);
    }
    std::vector<fw::spot_event_t> take_spot_events ()
    {
        std::lock_guard lock (mutex);
        std::vector<fw::spot_event_t> result{std::make_move_iterator (spot_events.begin ()),
                                             std::make_move_iterator (spot_events.end ())};
        spot_events.clear ();
        return result;
    }
};

node_state_t *g_node_state = nullptr;

inline void prepare_join (player_actor_t &actor, int x, int y, bool initial, bool crash = false)
{
    actor.pending_join = true;
    actor.pending_initial_entry = initial;
    actor.pending_crash_probe = crash;
    actor.pending_x = x;
    actor.pending_y = y;
    actor.pending_zone_id = zone_of (x, y);
    actor.context ()
      .join_spot (actor.pending_zone_id,
                  enter_zone_req_t{actor.player_id, x, y, actor.is_bot, initial,
                                   actor.initial_entry ? std::nullopt
                                                       : std::optional<std::string> (actor.zone_id),
                                   crash})
      .timeout (std::chrono::seconds (15))
      .defer ();
}

inline bool apply_move_authority (player_actor_t &actor, const move_msg_t &message)
{
    // join_spot is deferred: a later timer/message turn can run before its
    // completion callback. Keep exactly one relocation attempt per Actor so a
    // stale duplicate cannot race the accepted target restore.
    if (const auto error = validate_move (actor.x, actor.y, message.x, message.y, false)) {
        if (actor.is_bot) {
            actor.dir_x = -actor.dir_x;
            actor.dir_y = -actor.dir_y;
            std::cout << "zoneworld-bot-reversed player=" << actor.player_id << " reason=" << *error
                      << " position=" << actor.x << ',' << actor.y << " dir=" << actor.dir_x << ','
                      << actor.dir_y << std::endl;
        } else {
            actor.context ()
              .bound_session ()
              .send (move_rejected_notify_t{*error, actor.x, actor.y})
              .submit ();
        }
        return false;
    }
    const auto target = zone_of (message.x, message.y);
    if (target != actor.zone_id) {
        prepare_join (actor, message.x, message.y, false);
        return false;
    }
    actor.x = message.x;
    actor.y = message.y;
    return true;
}

class zone_entry_spot_t final : public fw::entry_spot_t<player_actor_t>
{
  public:
    explicit zone_entry_spot_t (fw::entry_spot_context_t context) : _context (std::move (context))
    {
    }
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_actor_send<&zone_entry_spot_t::join_world> (join_world_req_t::packet_name)
          .add_actor_request<&zone_entry_spot_t::enter_world> (enter_world_req_t::packet_name)
          .add_actor_send<&zone_entry_spot_t::move> (move_msg_t::packet_name)
          .add_actor_send<&zone_entry_spot_t::crash_probe> (
            crash_relocation_probe_msg_t::packet_name)
          .add_actor_request<&zone_entry_spot_t::location_probe> (
            actor_location_probe_req_t::packet_name)
          .add_actor_request<&zone_entry_spot_t::follow_probe> (
            message_follow_probe_req_t::packet_name)
          .add_actor_send<&zone_entry_spot_t::follow_probe_one_way> (
            message_follow_probe_msg_t::packet_name);
    }

    fw::task_t<fw::actor_create_response_t> on_create_actor (player_actor_t &,
                                                             const fw::message_t &) override
    {
        co_return fw::actor_create_response_t::accept ();
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (player_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (player_actor_t &) override { co_return; }

    void
    join_world (player_actor_t &actor, fw::message_context_t &, const join_world_req_t &request)
    {
        actor.player_id = request.player_id;
        if (!actor.initial_entry) {
            actor.context ()
              .bound_session ()
              .send (
                join_world_res_t{actor.player_id, actor.zone_id, actor.x, actor.y, std::nullopt})
              .submit ();
            return;
        }
        actor.x = spec_t::spawn_x;
        actor.y = spec_t::spawn_y;
        actor.zone_id = "zone-nw";
        actor.is_bot = false;
        prepare_join (actor, actor.x, actor.y, true);
    }

    enter_world_res_t
    enter_world (player_actor_t &actor, fw::message_context_t &, const enter_world_req_t &request)
    {
        actor.is_bot = request.is_bot;
        actor.dir_x = request.dir_x.value_or (0);
        actor.dir_y = request.dir_y.value_or (0);
        prepare_join (actor, request.x, request.y, true);
        return {zone_of (request.x, request.y), request.x, request.y, std::nullopt};
    }

    void move (player_actor_t &actor, fw::message_context_t &, const move_msg_t &message)
    {
        (void) apply_move_authority (actor, message);
    }

    void crash_probe (player_actor_t &actor,
                      fw::message_context_t &,
                      const crash_relocation_probe_msg_t &message)
    {
        prepare_join (actor, message.x, message.y, false, true);
    }

    message_follow_probe_res_t follow_probe (player_actor_t &actor,
                                             fw::message_context_t &,
                                             const message_follow_probe_req_t &request)
    {
        std::cout << "zoneworld-follow-request actor=" << actor.player_id
                  << " probe=" << request.probe_id
                  << " payload=" << nlohmann::json (request.payload).dump () << std::endl;
        return {request.probe_id, request.payload, std::nullopt};
    }

    actor_location_probe_res_t location_probe (player_actor_t &actor,
                                               fw::message_context_t &,
                                               const actor_location_probe_req_t &)
    {
        const auto &reference = actor.context ().actor_ref ();
        return {actor.player_id, reference.object_generation (),
                std::string (reference.node_rid ().value ()), std::nullopt};
    }

    void follow_probe_one_way (player_actor_t &actor,
                               fw::message_context_t &,
                               const message_follow_probe_msg_t &message)
    {
        std::cout << "zoneworld-follow-one-way actor=" << actor.player_id
                  << " probe=" << message.probe_id
                  << " payload=" << nlohmann::json (message.payload).dump () << std::endl;
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
          .add_actor_send<&zone_spot_t::join_world> (join_world_req_t::packet_name)
          .add_actor_send<&zone_spot_t::move> (move_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::bot_tick> (bot_tick_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::crash_probe> (crash_relocation_probe_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_state> (deliver_zone_state_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_changed> (deliver_zone_changed_msg_t::packet_name)
          .add_actor_send<&zone_spot_t::deliver_announce> (
            deliver_world_announce_msg_t::packet_name)
          .add_handler<&zone_spot_t::announce> (deliver_announce_msg_t::packet_name)
          .add_actor_request<&zone_spot_t::follow_probe> (message_follow_probe_req_t::packet_name)
          .add_actor_request<&zone_spot_t::location_probe> (actor_location_probe_req_t::packet_name)
          .add_actor_send<&zone_spot_t::follow_probe_one_way> (
            message_follow_probe_msg_t::packet_name);
        for (const auto &from : adjacent_zones (_context.spot_id ()))
            _context.handlers ().add_subscribe<&zone_spot_t::border> (
              border_topic (from, _context.spot_id ()));
    }

    fw::task_t<void> on_initialize () override
    {
        {
            std::lock_guard lock (g_node_state->mutex);
            g_node_state->zones.insert (_context.spot_id ());
        }
        _timer = _context.add_timer<zone_tick_handler_t> (
          "zone-tick", std::chrono::milliseconds (spec_t::tick_period_ms));
        _bot_timer = _context.add_timer<bot_tick_handler_t> (
          "bot-tick", std::chrono::milliseconds (spec_t::bot_tick_period_ms));
        std::cout << "zoneworld-zone-ready node=" << g_node_state->node_id
                  << " zone=" << _context.spot_id () << " owner=" << _context.node_rid ().value ()
                  << std::endl;
        co_return;
    }
    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &) override
    {
        co_return fw::spot_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        const auto enter = request.decode<enter_zone_req_t> ();
        if (enter.crash_boundary_probe) {
            std::cout << "zoneworld-crash-boundary join pending node=" << g_node_state->node_id
                      << " player=" << actor_id << std::endl;
            std::this_thread::sleep_for (std::chrono::seconds (60));
        }
        if (g_node_state->maintenance.load ()
            && (!enter.from_zone_id || *enter.from_zone_id != _context.spot_id ()))
            co_return fw::spot_actor_join_result_t::reject (
              enter_zone_res_t{_context.spot_id (), reject_reason_t::zone_maintenance});
        {
            std::lock_guard lock (_mutex);
            _pending[std::string (actor_id)] = enter;
        }
        co_return fw::spot_actor_join_result_t::accept (
          enter_zone_res_t{_context.spot_id (), std::nullopt});
    }

    fw::task_t<void> on_actor_joined (player_actor_t &actor) override
    {
        enter_zone_req_t enter;
        {
            std::lock_guard lock (_mutex);
            const auto found = _pending.find (actor.player_id);
            if (found == _pending.end ())
                co_return;
            enter = found->second;
            _pending.erase (found);
            _players[actor.player_id] = {actor.player_id, enter.x, enter.y, _context.spot_id (),
                                         enter.is_bot};
        }
        actor.x = enter.x;
        actor.y = enter.y;
        actor.zone_id = _context.spot_id ();
        actor.is_bot = enter.is_bot;
        {
            std::lock_guard lock (g_node_state->mutex);
            g_node_state->residents.insert (actor.player_id);
        }
        std::cout << "zoneworld-actor-joined node=" << g_node_state->node_id
                  << " zone=" << actor.zone_id << " player=" << actor.player_id
                  << " bot=" << (actor.is_bot ? "true" : "false")
                  << " initial=" << (enter.initial_entry ? "true" : "false") << std::endl;
        if (!actor.is_bot && !enter.initial_entry)
            actor.context ()
              .bound_session ()
              .send (zone_changed_notify_t{actor.player_id, actor.zone_id})
              .submit ();
        co_return;
    }

    fw::task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        {
            std::lock_guard lock (_mutex);
            _players.erase (actor.player_id);
        }
        {
            std::lock_guard lock (g_node_state->mutex);
            g_node_state->residents.erase (actor.player_id);
        }
        co_return;
    }

    fw::task_t<void>
    join_world (player_actor_t &actor, fw::message_context_t &, const join_world_req_t &)
    {
        co_await actor.context ()
          .bound_session ()
          .send (join_world_res_t{actor.player_id, actor.zone_id, actor.x, actor.y, std::nullopt})
          .submit ();
        std::cout << "zoneworld-join-response player=" << actor.player_id
                  << " zone=" << actor.zone_id << std::endl;
        co_return;
    }

    void move (player_actor_t &actor, fw::message_context_t &, const move_msg_t &message)
    {
        if (!apply_move_authority (actor, message))
            return;
        std::lock_guard lock (_mutex);
        _players[actor.player_id] = {actor.player_id, actor.x, actor.y, actor.zone_id,
                                     actor.is_bot};
    }

    void bot_tick (player_actor_t &actor, fw::message_context_t &, const bot_tick_msg_t &)
    {
        if (!actor.is_bot)
            return;
        if (apply_move_authority (actor, move_msg_t{actor.x + actor.dir_x * spec_t::bot_step,
                                                    actor.y + actor.dir_y * spec_t::bot_step})) {
            std::lock_guard lock (_mutex);
            _players[actor.player_id] = {actor.player_id, actor.x, actor.y, actor.zone_id, true};
        }
    }

    void crash_probe (player_actor_t &actor,
                      fw::message_context_t &,
                      const crash_relocation_probe_msg_t &message)
    {
        prepare_join (actor, message.x, message.y, false, true);
    }

    message_follow_probe_res_t follow_probe (player_actor_t &actor,
                                             fw::message_context_t &,
                                             const message_follow_probe_req_t &request)
    {
        std::cout << "zoneworld-follow-request actor=" << actor.player_id
                  << " probe=" << request.probe_id
                  << " payload=" << nlohmann::json (request.payload).dump () << std::endl;
        return {request.probe_id, request.payload, std::nullopt};
    }

    actor_location_probe_res_t location_probe (player_actor_t &actor,
                                               fw::message_context_t &,
                                               const actor_location_probe_req_t &)
    {
        const auto &reference = actor.context ().actor_ref ();
        return {actor.player_id, reference.object_generation (),
                std::string (reference.node_rid ().value ()), std::nullopt};
    }

    void follow_probe_one_way (player_actor_t &actor,
                               fw::message_context_t &,
                               const message_follow_probe_msg_t &message)
    {
        std::cout << "zoneworld-follow-one-way actor=" << actor.player_id
                  << " probe=" << message.probe_id
                  << " payload=" << nlohmann::json (message.payload).dump () << std::endl;
    }

    void deliver_state (player_actor_t &actor,
                        fw::message_context_t &,
                        const deliver_zone_state_msg_t &message)
    {
        if (!actor.is_bot)
            actor.context ()
              .bound_session ()
              .send (zone_state_notify_t{message.zone_id, message.tick, message.players})
              .submit ();
    }
    void deliver_changed (player_actor_t &actor,
                          fw::message_context_t &,
                          const deliver_zone_changed_msg_t &message)
    {
        if (!actor.is_bot)
            actor.context ()
              .bound_session ()
              .send (zone_changed_notify_t{message.player_id, message.zone_id})
              .submit ();
    }
    void deliver_announce (player_actor_t &actor,
                           fw::message_context_t &,
                           const deliver_world_announce_msg_t &message)
    {
        if (!actor.is_bot)
            actor.context ()
              .bound_session ()
              .send (world_announce_notify_t{message.announcement_id, message.text})
              .submit ();
    }

    fw::task_t<void> announce (const deliver_announce_msg_t &message)
    {
        std::cout << "zoneworld-zone-announcement node=" << g_node_state->node_id
                  << " zone=" << _context.spot_id () << " id=" << message.announcement_id
                  << std::endl;
        std::vector<std::string> humans;
        {
            std::lock_guard lock (_mutex);
            for (const auto &[id, player] : _players)
                if (!player.is_bot)
                    humans.push_back (id);
        }
        for (const auto &id : humans)
            co_await _actor_client
              .send (fw::actor_id_t (id),
                     deliver_world_announce_msg_t{message.announcement_id, message.text})
              .submit ();
    }

    void border (const zone_border_event_t &event)
    {
        std::lock_guard lock (_mutex);
        auto found = _borders.find (event.from_zone_id);
        if (found == _borders.end () || event.tick > found->second.event.tick) {
            _borders[event.from_zone_id] = border_state_t{event, _tick};
            if (std::any_of (event.players.begin (), event.players.end (),
                             [] (const auto &player) { return !player.is_bot; }))
                std::cout << "zoneworld-border-received zone=" << _context.spot_id ()
                          << " from=" << event.from_zone_id << " tick=" << event.tick
                          << " players=" << event.players.size () << std::endl;
        }
    }

    fw::task_t<void> tick ()
    {
        std::vector<player_view_t> local;
        std::vector<std::string> humans;
        {
            std::lock_guard lock (_mutex);
            ++_tick;
            for (const auto &[id, player] : _players) {
                local.push_back (player);
                if (!player.is_bot)
                    humans.push_back (id);
            }
        }
        if (!humans.empty () && g_node_state->fault_tick_zone
            && *g_node_state->fault_tick_zone == _context.spot_id ()
            && !g_node_state->fault_injected.exchange (true))
            throw std::runtime_error ("ZoneWorld injected zone tick failure");
        sort_players (local);
        for (const auto &to : adjacent_zones (_context.spot_id ())) {
            std::vector<player_view_t> border_players;
            std::copy_if (local.begin (), local.end (), std::back_inserter (border_players),
                          [] (const auto &player) {
                              return std::abs (player.x - spec_t::zone_split) <= spec_t::border_band
                                     || std::abs (player.y - spec_t::zone_split)
                                          <= spec_t::border_band;
                          });
            _context
              .publish (
                border_topic (_context.spot_id (), to),
                zone_border_event_t{_context.spot_id (), to, _tick, std::move (border_players)})
              .submit ();
        }

        std::map<std::string, player_view_t> merged;
        {
            std::lock_guard lock (_mutex);
            for (auto it = _borders.begin (); it != _borders.end ();) {
                if (_tick - it->second.received_at_tick >= spec_t::border_expiry_ticks) {
                    if (!it->second.event.players.empty ())
                        std::cout << "zoneworld-border-expired zone=" << _context.spot_id ()
                                  << " from=" << it->first << std::endl;
                    it = _borders.erase (it);
                } else {
                    for (const auto &player : it->second.event.players)
                        merged[player.player_id] = player;
                    ++it;
                }
            }
        }
        for (const auto &player : local)
            merged[player.player_id] = player;
        std::vector<player_view_t> snapshot;
        for (const auto &[_, player] : merged)
            snapshot.push_back (player);
        sort_players (snapshot);
        for (const auto &id : humans) {
            try {
                co_await _actor_client
                  .send (fw::actor_id_t (id),
                         deliver_zone_state_msg_t{_context.spot_id (), _tick, snapshot})
                  .submit ();
            }
            catch (const fw::framework_exception_t &) {
            }
        }
        co_return;
    }

    fw::task_t<void> tick_bots ()
    {
        std::vector<std::string> bots;
        {
            std::lock_guard lock (_mutex);
            for (const auto &[id, player] : _players)
                if (player.is_bot)
                    bots.push_back (id);
        }
        for (const auto &id : bots)
            co_await _actor_client.send (fw::actor_id_t (id), bot_tick_msg_t{}).submit ();
    }

  private:
    struct border_state_t
    {
        zone_border_event_t event;
        std::int64_t received_at_tick = 0;
    };
    fw::spot_context_t _context;
    fw::actor_client_t &_actor_client;
    fw::timer_t _timer;
    fw::timer_t _bot_timer;
    std::mutex _mutex;
    std::map<std::string, enter_zone_req_t> _pending;
    std::map<std::string, player_view_t> _players;
    std::map<std::string, border_state_t> _borders;
    std::int64_t _tick = 0;
};

struct zone_tick_handler_t
{
    fw::task_t<void> handle (zone_spot_t &spot, const fw::timer_tick_t &) const
    {
        return spot.tick ();
    }
};
struct bot_tick_handler_t
{
    fw::task_t<void> handle (zone_spot_t &spot, const fw::timer_tick_t &) const
    {
        return spot.tick_bots ();
    }
};

class apply_maintenance_handler_t
{
  public:
    apply_node_maintenance_res_t handle (const apply_node_maintenance_req_t &request)
    {
        if (request.node_id != g_node_state->node_id)
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "maintenance target does not match this node");
        g_node_state->maintenance.store (request.enabled);
        return {request.node_id, request.enabled, g_node_state->zone_snapshot ()};
    }
};

class diagnostics_handler_t
{
  public:
    get_node_diagnostics_res_t handle (const get_node_diagnostics_req_t &request)
    {
        if (request.node_id != g_node_state->node_id)
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "diagnostics target does not match this node");
        return {request.node_id, g_node_state->zone_snapshot (), g_node_state->player_count (),
                g_node_state->maintenance.load ()};
    }
};

struct maintenance_fanout_handler_t
{
    using event_type = node_maintenance_changed_event_t;
    static constexpr const char *topic_name = names_t::maintenance_topic;
    void handle (const node_maintenance_changed_event_t &event,
                 const fw::publish_message_context_t &)
    {
        if (event.node_id == g_node_state->node_id)
            g_node_state->maintenance.store (event.enabled);
    }
};

class announce_fanout_handler_t
{
  public:
    using event_type = world_announce_event_t;
    using dependency_types = fw::dependency_list_t<fw::route_client_t>;
    static constexpr const char *topic_name = names_t::announce_topic;
    explicit announce_fanout_handler_t (fw::route_client_t &routes) : _routes (routes) {}
    fw::task_t<void> handle (const world_announce_event_t &event,
                             const fw::publish_message_context_t &)
    {
        std::cout << "zoneworld-fanout-announcement node=" << g_node_state->node_id
                  << " id=" << event.announcement_id << std::endl;
        for (const auto &zone : g_node_state->zone_snapshot ())
            co_await _routes
              .send_to_spot (fw::spot_id_t (zone),
                             deliver_announce_msg_t{event.announcement_id, event.text})
              .submit ();
    }

  private:
    fw::route_client_t &_routes;
};

struct extra_announce_handler_t
{
    using event_type = world_announce_event_t;
    static constexpr const char *topic_name = names_t::announce_topic;
    void handle (const world_announce_event_t &event, const fw::publish_message_context_t &)
    {
        std::cout << "zoneworld-extra-subscriber-announcement id=" << event.announcement_id
                  << std::endl;
    }
};

class node_report_service_t final : public fw::hosted_service_t
{
  public:
    void start (fw::service_provider_t &services) override
    {
        _routes = &services.get_required<fw::route_client_t> ();
        _running.store (true);
        _worker = std::thread ([this] {
            auto next_status_report = std::chrono::steady_clock::now ();
            while (_running.load ()) {
                std::erase_if (_work,
                               [] (const fw::task_t<void> &work) { return work.await_ready (); });
                for (auto &event : g_node_state->take_spot_events ())
                    _work.push_back (report_spot_event (std::move (event)));
                const auto now = std::chrono::steady_clock::now ();
                if (now >= next_status_report) {
                    _work.push_back (report_once ());
                    next_status_report = now + std::chrono::milliseconds (spec_t::report_period_ms);
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
            }
        });
    }
    void request_stop () noexcept override { _running.store (false); }
    void stop () noexcept override
    {
        request_stop ();
        if (_worker.joinable ())
            _worker.join ();
        _work.clear ();
        _routes = nullptr;
    }

  private:
    static std::string format_timestamp (std::chrono::system_clock::time_point timestamp)
    {
        const auto time = std::chrono::system_clock::to_time_t (timestamp);
        std::tm utc{};
        gmtime_r (&time, &utc);
        std::ostringstream value;
        value << std::put_time (&utc, "%Y-%m-%dT%H:%M:%SZ");
        return value.str ();
    }

    fw::task_t<void> report_spot_event (fw::spot_event_t event)
    {
        try {
            const auto kind = event.event == fw::spot_event_kind_t::timer_handler_failed
                                ? "TimerHandlerFailed"
                                : "TimerStoppedAfterUnhandledException";
            const auto detail = "spot=" + std::string (event.diagnostic.spot_id)
                                + "; timer=" + event.diagnostic.timer_name
                                + "; detail=" + event.diagnostic.message;
            co_await _routes
              ->send_to_channel (names_t::report_channel,
                                 report_spot_event_msg_t{g_node_state->node_id, kind, detail,
                                                         format_timestamp (event.timestamp)})
              .submit ();
            std::cout << "zoneworld-spot-event-reported node=" << g_node_state->node_id
                      << " kind=" << kind << " " << detail << std::endl;
        }
        catch (const std::exception &error) {
            std::cerr << "zoneworld-spot-event-report-failed node=" << g_node_state->node_id
                      << " error=" << error.what () << '\n';
        }
    }

    fw::task_t<void> report_once ()
    {
        try {
            co_await _routes
              ->send_to_channel (names_t::report_channel,
                                 report_node_status_msg_t{g_node_state->node_id,
                                                          g_node_state->zone_snapshot (),
                                                          g_node_state->player_count (),
                                                          g_node_state->maintenance.load ()})
              .submit ();
            std::cout << "zoneworld-status-report node=" << g_node_state->node_id << std::endl;
        }
        catch (const std::exception &error) {
            std::cerr << "zoneworld-status-report-failed node=" << g_node_state->node_id
                      << " error=" << error.what () << '\n';
        }
    }

    fw::route_client_t *_routes = nullptr;
    std::atomic_bool _running{false};
    std::thread _worker;
    std::vector<fw::task_t<void>> _work;
};

} // namespace zlink::samples::zoneworld

int main (int argc, char **argv)
{
    using namespace zlink::samples::zoneworld;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    const auto configuration = load_configuration (app, argc, argv);
    maintenance_store_t maintenance (configuration);
    node_state_t state{configuration.node_id, configuration.fault_tick_zone};
    state.maintenance.store (maintenance.read (configuration.node_id));
    g_node_state = &state;
    app.logging ().use_console ().set_min_level (fw::log_level_t::info);
    app.monitoring ()
      .add_spot_events (names_t::mesh)
      .on_spot_event (
        [] (const fw::spot_event_t &event) { g_node_state->record_spot_event (event); });
    auto &options = app.add_zlink_framework ();
    if (configuration.subscriber_only) {
        options.handlers ()
          .group ("zoneworld-extra-broadcast")
          .add_publish<extra_announce_handler_t> ();
        options.add_fanout_channel (names_t::broadcast_channel)
          .enable_subscriber ()
          .use_handler_group ("zoneworld-extra-broadcast");
    } else {
        options.add_location_store<fw::redis::redis_location_store_t> ()
          .set_connection_string (configuration.redis_endpoint)
          .set_key_prefix (configuration.redis_key_prefix + "location:");
        options.add_relocation_store<fw::redis::redis_relocation_store_t> ()
          .set_connection_string (configuration.redis_endpoint)
          .set_key_prefix (configuration.redis_key_prefix + "relocation:");
        auto mesh = options.add_route_mesh (names_t::mesh);
        mesh.set_automatic_routing_id_prefix ("zn");
        if (configuration.mesh_advertise_host)
            mesh.set_advertise_host (*configuration.mesh_advertise_host);
        mesh.listen (configuration.mesh_endpoint);
        mesh.channel (names_t::zone_channel).server ();
        mesh.channel (names_t::report_channel).client ();
        mesh.channel (names_t::ops_channel (configuration.node_id))
          .server ()
          .add_request_handler<apply_maintenance_handler_t, apply_node_maintenance_req_t,
                               apply_node_maintenance_res_t> ()
          .add_request_handler<diagnostics_handler_t, get_node_diagnostics_req_t,
                               get_node_diagnostics_res_t> ();
        mesh.objects ()
          .server ()
          .add_entry_spot<zone_entry_spot_t> ()
          .add_spot_factory<zone_spot_t, fw::actor_client_t> (names_t::zone_spot)
          .set_stable_type_limit (2)
          .disable_relocation ()
          .add_actor_factory<player_actor_t, player_actor_factory_t> (names_t::player_actor)
          .preserve_state_with<player_relocation_adapter_t> ();
        options.handlers ()
          .group ("zoneworld-broadcast")
          .add_publish<maintenance_fanout_handler_t> ()
          .add_publish<announce_fanout_handler_t> ();
        options.add_fanout_channel (names_t::broadcast_channel)
          .enable_subscriber ()
          .use_handler_group ("zoneworld-broadcast");
        options.http ().listen (configuration.bootstrap_http_endpoint).map_health ("/health");
    }
    if (!configuration.subscriber_only)
        app.add_hosted_service (std::make_unique<node_report_service_t> ());
    std::cout << "zoneworld-role-ready role=zone-node node=" << configuration.node_id
              << " subscriber-only=" << (configuration.subscriber_only ? "true" : "false")
              << std::endl;
    return app.run (argc, argv);
}
