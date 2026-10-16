#pragma once
#include "server/shared/player.hpp"
#include "domain.hpp"
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

  public:
    FieldState state;
    Field (fx::spot_context_t context,
           fx::actor_client_t &actors,
           fx::spot_manager_t &spots,
           fx::spot_publisher_client_t &publisher,
           const NodeConfig &config) :
        context_ (std::move (context)),
        actors_ (actors),
        spots_ (spots),
        publisher_ (publisher),
        config_ (config)
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
          .add_actor_send<&Field::deliver_field> ();
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
        const auto arrival = payload.decode<EnterField> ();
        if (Names::field_id (arrival.x) != context_.spot_id ())
            co_return fx::spot_actor_join_result_t::reject ();
        if (!state.arrivals.emplace (id, arrival).second)
            throw std::runtime_error ("Duplicate field admission");
        co_return fx::spot_actor_join_result_t::accept ();
    }
    void remember (const Player &actor)
    {
        state.visiblePlayers[std::string (actor.context ().actor_id ().value ())] =
          EnterField{actor.state.x, actor.state.sequence};
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
        state.leave (std::string (actor.context ().actor_id ().value ()));
        co_return;
    }
    fx::task_t<void> on_disconnect_actor (Player &actor) override
    {
        state.visiblePlayers.erase (std::string (actor.context ().actor_id ().value ()));
        co_return;
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
        const EnterField destination{request.x, request.sequence};
        if (Names::field_id (request.x) != context_.spot_id ()) {
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
    FieldView view () const
    {
        FieldView result{context_.spot_id (), config_.Id, {}, {}};
        for (const auto &[id, position] : state.visiblePlayers)
            result.players.push_back (id);
        for (const auto &[id, members] : state.parties)
            result.parties.push_back ({id, members});
        return result;
    }
    fx::task_t<void> party (Player &actor, const std::string &id, bool create)
    {
        const auto player = std::string (actor.context ().actor_id ().value ());
        if (!state.party (player, id, create)) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::invalid_party})
              .async ();
            co_return;
        }
        const auto result = view ();
        co_await actor.context ().bound_session ().send (result).async ();
        for (const auto &[other, position] : state.visiblePlayers)
            if (other != player)
                co_await actors_.send (fx::actor_id_t (other), result).async ();
    }
    fx::task_t<void>
    create_party (Player &actor, fx::message_context_t &, const CreatePartyRequest &request)
    {
        co_await party (actor, request.partyId, true);
    }
    fx::task_t<void>
    join_party (Player &actor, fx::message_context_t &, const JoinPartyRequest &request)
    {
        co_await party (actor, request.partyId, false);
    }
    fx::task_t<void> deliver_field (Player &actor, fx::message_context_t &, const FieldView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void>
    enter_dungeon (Player &actor, fx::message_context_t &, const EnterDungeonRequest &)
    {
        const auto player = actor.context ().actor_id ().value ();
        const auto party =
          std::find_if (state.parties.begin (), state.parties.end (),
                        [player] (const auto &entry) { return entry.second.front () == player; });
        if (party == state.parties.end ()) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::not_leader})
              .async ();
            co_return;
        }
        const auto partyId = party->first;
        const auto members = party->second;
        const auto dungeonId = Names::dungeon_id (partyId);
        const auto created =
          co_await spots_.get_or_create (dungeonId, Names::dungeon).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto prepared =
          co_await context_.request_to_spot (dungeonId, PrepareDungeon{partyId, members})
            .async<DungeonPrepared> ();
        if (prepared.dungeonId != dungeonId)
            throw std::runtime_error ("Dungeon preparation identity mismatch");
        for (const auto &id : members)
            co_await actors_.send (fx::actor_id_t (id), JoinDungeon{dungeonId, partyId}).async ();
    }
    fx::task_t<void> publish_border ()
    {
        const auto adjacent_fields = Names::adjacent (context_.spot_id ());
        for (const auto &[id, position] : state.visiblePlayers) {
            const auto offset = position.x % Names::field_width;
            if (offset >= Names::border_radius
                && offset < Names::field_width - Names::border_radius)
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
        for (const auto &[id, observer] : state.visiblePlayers) {
            if (std::abs (observer.x - event.x) > Names::border_radius)
                continue;
            co_await actors_
              .send (fx::actor_id_t (id), BorderVisible{context_.spot_id (), event.sourceFieldId,
                                                        event.playerId, event.x, event.sequence})
              .async ();
        }
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
        const auto payload = zlink::message_t::from_json (field.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Field &field, std::vector<std::byte> payload, std::stop_token) override
    {
        field.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<FieldState> ();
        co_return;
    }
};
}
