#pragma once
#include "server/shared/player.hpp"
#include "domain.hpp"
#include "party-domain.hpp"
#include <cstdlib>
#include <iostream>

namespace MyGame
{
class Field;
struct BorderTick
{
    fx::task_t<void> handle (Field &field, const fx::timer_tick_t &);
};
class Field final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;
    fx::spot_publisher_client_t &publisher_;
    const NodeConfig &config_;
    std::map<std::string, EnterField> visible_players_;
    FieldPositionDomain positions_;
    template <class Registry>
    static void subscribe_border (Registry registry, const std::string &topic)
    {
        if constexpr (requires {
                          registry.template add_subscribe<&Field::border> (Names::border, topic);
                      })
            registry.template add_subscribe<&Field::border> (Names::border, topic);
        else {
            constexpr auto reason =
              "Framework named-channel Spot subscription API is "
              "unavailable (#1783); border requires add_subscribe(channel, topic)";
            std::cerr << reason << std::endl;
            throw std::runtime_error (reason);
        }
    }

    fx::task_t<void> reject (Player &actor, const std::string &reason)
    {
        co_await actor.context ().bound_session ().send (ActionRejected{reason}).async ();
    }

  public:
    Field (fx::spot_context_t context,
           fx::actor_client_t &actors,
           fx::spot_manager_t &spots,
           fx::spot_publisher_client_t &publisher,
           const NodeConfig &config) :
        context_ (std::move (context)),
        actors_ (actors),
        spots_ (spots),
        publisher_ (publisher),
        config_ (config),
        positions_ (context_.spot_id ())
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Field::reconnect> ()
          .add_actor_send<&Field::move> ()
          .add_actor_send<&Field::deliver> ()
          .add_actor_send<&Field::create_party> ()
          .add_actor_send<&Field::join_party> ()
          .add_actor_send<&Field::enter_dungeon> ()
          .add_actor_send<&Field::travel> ()
          .add_actor_send<&Field::deliver_field> ()
          .add_actor_send<&Field::query> ();
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
        const auto rejection = positions_.queue_arrival (
          std::string (id), payload.decode<EnterField> ());
        co_return rejection ? fx::spot_actor_join_result_t::reject ()
                            : fx::spot_actor_join_result_t::accept ();
    }
    void remember (const Player &actor)
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        positions_.set_connected (id, true);
        visible_players_[id] = EnterField{actor.state.x, actor.state.sequence};
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        EnterField arrival;
        if (!positions_.try_take_arrival (id, arrival))
            throw std::runtime_error ("Field admission has no movement intent");
        positions_.add_player (id, arrival);
        actor.state.apply (arrival);
        remember (actor);
        co_return;
    }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        visible_players_.erase (id);
        positions_.remove_player (id);
        co_return;
    }
    fx::task_t<void> on_disconnect_actor (Player &actor) override
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        visible_players_.erase (id);
        positions_.set_connected (id, false);
        co_return;
    }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        remember (actor);
        co_await actor.send_state ();
        if (actor.state.partyId)
            co_await request_party_view (actor, *actor.state.partyId);
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        const auto result = positions_.move (id, request);
        if (result.rejection) {
            co_await actor.context ().bound_session ().send (*result.rejection).async ();
            co_return;
        }
        const EnterField destination{result.position.x, result.sequence, result.remainingMs};
        if (result.relocates) {
            actor.move (destination);
            co_return;
        }
        actor.state.apply (destination);
        remember (actor);
        co_await actor.send_state ();
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const BorderVisible &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> deliver_field (Player &actor, fx::message_context_t &, const PartyView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> request_party_view (Player &actor, const std::string &party_id)
    {
        const auto response = co_await context_.request_to_spot (
                                party_id,
                                PartyCommand{std::string (actor.context ().actor_id ().value ())})
                                .async<PartyCommandResult> ();
        if (response.view)
            co_await actor.context ().bound_session ().send (*response.view).async ();
    }
    fx::task_t<void> show (Player &actor)
    {
        co_await actor.send_state ();
        if (actor.state.partyId)
            co_await request_party_view (actor, *actor.state.partyId);
    }
    fx::task_t<void> query (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await show (actor);
    }
    fx::task_t<void> create_party (Player &actor,
                                   fx::message_context_t &,
                                   const CreatePartyRequest &)
    {
        if (actor.state.partyId) {
            co_await reject (actor, Rejection::invalid_party);
            co_return;
        }
        const auto party_id = Names::new_id ("party-");
        const auto created = co_await spots_.get_or_create (party_id, Names::party)
                               .in_mesh (Names::mesh)
                               .async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Party creation rejected");
        co_await join_party_with_id (actor, party_id);
    }
    fx::task_t<void>
    join_party (Player &actor, fx::message_context_t &, const JoinPartyRequest &request)
    {
        if (actor.state.partyId || !PartyRun::valid_id (request.partyId, "party-")) {
            co_await reject (actor, Rejection::invalid_party);
            co_return;
        }
        const auto found = co_await spots_.find (request.partyId);
        if (!found) {
            co_await reject (actor, Rejection::invalid_party);
            co_return;
        }
        co_await join_party_with_id (actor, request.partyId);
    }
    fx::task_t<void> join_party_with_id (Player &actor, const std::string &party_id)
    {
        const auto response = co_await context_.request_to_spot (
                                party_id,
                                PartyCommand{std::string (actor.context ().actor_id ().value ()), true})
                                .async<PartyCommandResult> ();
        if (response.rejection) {
            co_await actor.context ().bound_session ().send (*response.rejection).async ();
            co_return;
        }
        actor.state.partyId = party_id;
    }
    fx::task_t<void> enter_dungeon (Player &actor,
                                    fx::message_context_t &,
                                    const EnterDungeonRequest &)
    {
        if (!actor.state.partyId) {
            co_await reject (actor, Rejection::not_leader);
            co_return;
        }
        const auto response = co_await context_.request_to_spot (
                                *actor.state.partyId,
                                PartyCommand{std::string (actor.context ().actor_id ().value ()), false, true})
                                .async<PartyCommandResult> ();
        if (response.rejection)
            co_await actor.context ().bound_session ().send (*response.rejection).async ();
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void> publish_border ()
    {
        const auto adjacent_fields = Names::adjacent (context_.spot_id ());
        for (const auto &[id, position] : visible_players_) {
            const auto offset = std::fmod (position.x, Names::field_width);
            if (offset >= Names::border_radius && offset < Names::field_width - Names::border_radius)
                continue;
            for (const auto &adjacent : adjacent_fields)
                co_await publisher_
                  .publish (Names::border, Names::topic (context_.spot_id (), adjacent),
                            BorderPresence{context_.spot_id (), id, position.x, position.sequence})
                  .async ();
        }
    }
    fx::task_t<void> border (const BorderPresence &event)
    {
        for (const auto &[id, observer] : visible_players_) {
            if (std::abs (observer.x - event.x) > Names::border_radius)
                continue;
            co_await actors_
              .send (fx::actor_id_t (id), BorderVisible{context_.spot_id (), event.sourceFieldId,
                                                        event.playerId, event.x, event.sequence})
              .async ();
        }
    }
    FieldSnapshot capture () const
    {
        return {visible_players_, positions_.capture ()};
    }
    void restore (const FieldSnapshot &snapshot)
    {
        positions_.restore (snapshot.positions);
        visible_players_ = snapshot.visiblePlayers;
    }
};
inline fx::task_t<void> BorderTick::handle (Field &field, const fx::timer_tick_t &)
{
    co_await field.publish_border ();
}
class FieldRelocation final : public fx::spot_relocation_adapter_t<Field>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Field &field, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (field.capture ());
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Field &field, std::vector<std::byte> payload, std::stop_token) override
    {
        const auto snapshot =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<FieldSnapshot> ();
        field.restore (snapshot);
        co_return;
    }
};
}
