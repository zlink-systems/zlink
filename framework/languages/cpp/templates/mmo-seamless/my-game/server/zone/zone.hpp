#pragma once
#include "player.hpp"
#include <cstdlib>
#include <iostream>

namespace MyGame
{
class Zone;
struct BorderTick
{
    fx::task_t<void> handle (Zone &zone, const fx::timer_tick_t &);
};
class Zone final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;
    fx::spot_publisher_client_t &publisher_;
    template <class Registry>
    static void subscribe_border (Registry registry, const std::string &topic)
    {
        if constexpr (requires {
                          registry.template add_subscribe<&Zone::border> (Names::border, topic);
                      })
            registry.template add_subscribe<&Zone::border> (Names::border, topic);
        else {
            constexpr auto reason = "Framework named-channel Spot subscription API is "
                                    "unavailable; border requires add_subscribe(channel, topic)";
            std::cerr << reason << std::endl;
            throw std::runtime_error (reason);
        }
    }

  public:
    ZoneState state;
    Zone (fx::spot_context_t context,
          fx::actor_client_t &actors,
          fx::spot_manager_t &spots,
          fx::spot_publisher_client_t &publisher) :
        context_ (std::move (context)), actors_ (actors), spots_ (spots), publisher_ (publisher)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Zone::reconnect> ()
          .add_actor_send<&Zone::move> ()
          .add_actor_send<&Zone::deliver> ()
          .add_actor_send<&Zone::dungeon> ();
        for (const auto &adjacent : Names::adjacent (context_.spot_id ()))
            subscribe_border (context_.handlers (), Names::topic (adjacent, context_.spot_id ()));
    }
    fx::task_t<void> on_initialize () override
    {
        context_.add_timer<BorderTick> (Names::timer, Names::tick_period);
        co_return;
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto arrival = payload.decode<EnterZone> ();
        if (Names::zone_id (arrival.x) != context_.spot_id ())
            co_return fx::spot_actor_join_result_t::reject ();
        if (!state.arrivals.emplace (id, arrival).second)
            throw std::runtime_error ("Duplicate zone admission");
        co_return fx::spot_actor_join_result_t::accept ();
    }
    void remember (const Player &actor)
    {
        state.visiblePlayers[std::string (actor.context ().actor_id ().value ())] =
          EnterZone{actor.state.x, actor.state.sequence};
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        const auto arrival = state.arrivals.at (id);
        state.arrivals.erase (id);
        actor.state.apply (arrival);
        remember (actor);
        co_return;
    }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        state.visiblePlayers.erase (std::string (actor.context ().actor_id ().value ()));
        co_return;
    }
    fx::task_t<void> on_disconnect_actor (Player &actor) override
    {
        co_await on_leave_actor (actor);
    }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        remember (actor);
        co_await actor.send_state ();
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        actor.state.validate (request);
        const EnterZone destination{request.x, request.sequence};
        if (Names::zone_id (request.x) != context_.spot_id ()) {
            actor.move (destination);
            co_return;
        }
        actor.state.apply (destination);
        remember (actor);
        co_await actor.send_state ();
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const BorderVisible &event)
    {
        // Membership or visibility may change before a queued notification is handled.
        if (actor.context ().spot_id () != event.observerZoneId
            || !state.visiblePlayers.contains (std::string (actor.context ().actor_id ().value ())))
            co_return;
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void>
    dungeon (Player &actor, fx::message_context_t &, const OpenDungeonRequest &request)
    {
        const auto created = co_await spots_.get_or_create (request.dungeonId, Names::dungeon)
                               .in_mesh (Names::mesh)
                               .async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto opened =
          co_await context_.request_to_spot (request.dungeonId, request).async<DungeonOpened> ();
        co_await actor.context ().bound_session ().send (opened).async ();
    }
    fx::task_t<void> publish_border ()
    {
        const auto adjacent_zones = Names::adjacent (context_.spot_id ());
        for (const auto &[id, position] : state.visiblePlayers) {
            const auto offset = position.x % Names::zone_width;
            if (offset >= Names::border_radius && offset < Names::zone_width - Names::border_radius)
                continue;
            for (const auto &adjacent : adjacent_zones)
                co_await publisher_
                  .publish (Names::border, Names::topic (context_.spot_id (), adjacent),
                            BorderPresence{context_.spot_id (), id, position.x, position.sequence})
                  .async ();
        }
    }
    fx::task_t<void> border (const BorderPresence &event)
    {
        for (const auto &[id, observer] : state.visiblePlayers) {
            if (std::abs (observer.x - event.x) > Names::border_radius)
                continue;
            co_await actors_
              .send (fx::actor_id_t (id), BorderVisible{context_.spot_id (), event.sourceZoneId,
                                                        event.actorId, event.x, event.sequence})
              .async ();
        }
    }
};
inline fx::task_t<void> BorderTick::handle (Zone &zone, const fx::timer_tick_t &)
{
    co_await zone.publish_border ();
}
class ZoneRelocation final : public fx::spot_relocation_adapter_t<Zone>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Zone &zone, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (zone.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Zone &zone, std::vector<std::byte> payload, std::stop_token) override
    {
        zone.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<ZoneState> ();
        co_return;
    }
};
}
