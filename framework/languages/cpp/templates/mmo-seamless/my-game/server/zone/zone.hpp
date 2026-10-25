#pragma once
#include "player.hpp"
#include "server/shared/config.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace MyGame
{
class Zone final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;
    fx::spot_publisher_client_t &publisher_;
    const NodeConfig &config_;
    ZonePositionDomain positions_;
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

    template <class Event>
    fx::task_t<void> publish_local (const std::string &player_id, const Event &event)
    {
        for (const auto &[observer_id, observer] : positions_.connected_players ()) {
            (void) observer;
            if (observer_id == player_id)
                continue;
            co_await actors_.send (fx::actor_id_t (observer_id), event).async ();
        }
    }

    fx::task_t<void> publish_border (const std::string &player_id,
                                     const Position &position,
                                     std::int64_t sequence,
                                     BorderPresenceKind kind,
                                     std::optional<PlayerLeftReason> reason)
    {
        const auto offset = std::fmod (position.x, Names::zone_width);
        if (offset >= Names::border_radius && offset < Names::zone_width - Names::border_radius)
            co_return;
        for (const auto &adjacent : Names::adjacent (context_.spot_id ()))
            co_await publisher_
              .publish (Names::border,
                        Names::topic (context_.spot_id (), adjacent),
                        BorderPresence{context_.spot_id (), player_id, position, sequence, kind,
                                       reason})
              .async ();
    }

    fx::task_t<void> publish_left (const std::string &player_id,
                                  const ZonePositionDomain::PlayerView &state,
                                  PlayerLeftReason reason)
    {
        co_await publish_local (player_id, PlayerLeft{player_id, reason});
        co_await publish_border (
          player_id, state.position, state.sequence, BorderPresenceKind::left, reason);
    }

  public:
    ZonePositionDomain::Snapshot capture_state () const { return positions_.capture (); }
    void restore_state (const ZonePositionDomain::Snapshot &state) { positions_.restore (state); }

    Zone (fx::spot_context_t context,
          fx::actor_client_t &actors,
          fx::spot_manager_t &spots,
          fx::spot_publisher_client_t &publisher,
          const NodeConfig &config) :
        context_ (std::move (context)),
        actors_ (actors),
        spots_ (spots),
        publisher_ (publisher),
        config_ (config),
        positions_ (std::string (context_.spot_id ()))
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Zone::reconnect> ()
          .add_actor_send<&Zone::move> ()
          .add_actor_send<&Zone::deliver_appeared> ()
          .add_actor_send<&Zone::deliver_moved> ()
          .add_actor_send<&Zone::deliver_left> ()
          .add_actor_send<&Zone::dungeon> ();
        for (const auto &adjacent : Names::adjacent (context_.spot_id ()))
            subscribe_border (context_.handlers (), Names::topic (adjacent, context_.spot_id ()));
    }
    fx::task_t<void> on_initialize () override { co_return; }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto arrival = payload.decode<EnterZone> ();
        const auto rejection = positions_.queue_arrival (std::string (id), arrival);
        if (rejection)
            co_return fx::spot_actor_join_result_t::reject (*rejection);
        co_return fx::spot_actor_join_result_t::accept (
          WorldState{context_.spot_id (), config_.Id, arrival.position.x, arrival.sequence});
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        EnterZone arrival;
        if (!positions_.take_arrival (id, arrival))
            throw std::runtime_error ("Zone admission has no movement intent");
        positions_.add_player (id, arrival);
        co_await publish_local (id, PlayerAppeared{id, arrival.position});
        co_await publish_border (id, arrival.position, arrival.sequence,
                                 BorderPresenceKind::appeared, std::nullopt);
    }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        const auto state = positions_.remove_player (id);
        if (state)
            co_await publish_left (id, *state, PlayerLeftReason::left_zone);
    }
    fx::task_t<void> on_disconnect_actor (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        ZonePositionDomain::PlayerView previous;
        if (!positions_.try_state (id, previous) || !previous.connected)
            co_return;
        const auto state = positions_.set_connected (id, false);
        if (state)
            co_await publish_left (id, *state, PlayerLeftReason::disconnected);
    }
    fx::task_t<void> reconnect (Player &actor,
                                fx::message_context_t &,
                                const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        ZonePositionDomain::PlayerView previous;
        if (!positions_.try_state (std::string (actor.context ().actor_id ().value ()), previous)) {
            co_await actor.context ().bound_session ().send (ActionRejected{"outside-world", 0}).async ();
            co_return;
        }
        const auto id = std::string (actor.context ().actor_id ().value ());
        const auto state = positions_.set_connected (id, true).value ();
        if (!previous.connected) {
            co_await publish_local (id, PlayerAppeared{id, state.position});
            co_await publish_border (id, state.position, state.sequence,
                                     BorderPresenceKind::appeared, std::nullopt);
        }
        co_await actor.context ()
          .bound_session ()
          .send (WorldState{context_.spot_id (), config_.Id, state.position.x, state.sequence})
          .async ();
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        const auto movement = positions_.move (id, request);
        if (movement.rejection) {
            co_await actor.context ().bound_session ().send (*movement.rejection).async ();
            co_return;
        }
        const EnterZone destination{movement.position, movement.sequence, movement.remainingMs};
        if (movement.relocates) {
            actor.move (destination);
            co_return;
        }
        co_await publish_local (id, PlayerMoved{id, movement.position, movement.sequence});
        co_await publish_border (id, movement.position, movement.sequence,
                                 BorderPresenceKind::moved, std::nullopt);
        co_await actor.context ()
          .bound_session ()
          .send (WorldState{context_.spot_id (), config_.Id, movement.position.x,
                            movement.sequence})
          .async ();
    }
    fx::task_t<void> deliver_appeared (Player &actor,
                                       fx::message_context_t &,
                                       const PlayerAppeared &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> deliver_moved (Player &actor,
                                    fx::message_context_t &,
                                    const PlayerMoved &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> deliver_left (Player &actor,
                                   fx::message_context_t &,
                                   const PlayerLeft &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> border (const BorderPresence &event)
    {
        for (const auto &[id, observer] : positions_.connected_players ()) {
            if (id == event.playerId
                || std::abs (observer.position.x - event.position.x) > Names::border_radius)
                continue;
            if (event.kind == BorderPresenceKind::appeared)
                co_await actors_
                  .send (fx::actor_id_t (id), PlayerAppeared{event.playerId, event.position})
                  .async ();
            else if (event.kind == BorderPresenceKind::moved)
                co_await actors_
                  .send (fx::actor_id_t (id),
                         PlayerMoved{event.playerId, event.position, event.sequence})
                  .async ();
            else if (event.kind == BorderPresenceKind::left)
                co_await actors_
                  .send (fx::actor_id_t (id),
                         PlayerLeft{event.playerId, event.reason.value_or (
                                                    PlayerLeftReason::left_zone)})
                  .async ();
            else
                throw std::invalid_argument ("Unknown border presence kind");
        }
    }
    fx::task_t<void> dungeon (Player &actor,
                              fx::message_context_t &,
                              const OpenDungeonRequest &request)
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
};
class ZoneRelocation final : public fx::spot_relocation_adapter_t<Zone>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Zone &zone, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (zone.capture_state ());
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Zone &zone, std::vector<std::byte> payload, std::stop_token) override
    {
        zone.restore_state (
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<
            ZonePositionDomain::Snapshot> ());
        co_return;
    }
};
}
