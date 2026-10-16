#pragma once
#include "player.hpp"
#include "server/region/domain/positions.hpp"
namespace MyGame
{
class Region final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;
    RegionView view () const { return {context_.spot_id (), config_.Id, positions.view ()}; }
    fx::task_t<void> broadcast (Player *requester = nullptr)
    {
        const auto update = view ();
        for (const auto &[id, position] : positions.players ()) {
            if (requester && requester->context ().actor_id ().value () == id)
                co_await requester->context ().bound_session ().send (update).async ();
            else
                co_await actors_.send (fx::actor_id_t (id), update).async ();
        }
    }

  public:
    Positions positions;
    Region (fx::spot_context_t context,
            const NodeConfig &config,
            fx::actor_client_t &actors,
            fx::spot_manager_t &spots) :
        context_ (std::move (context)), config_ (config), actors_ (actors), spots_ (spots)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Region::enter> ()
          .add_actor_send<&Region::show> ()
          .add_actor_send<&Region::move> ()
          .add_actor_send<&Region::deliver> ()
          .add_actor_send<&Region::dungeon> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto join = payload.decode<JoinRegion> ();
        if (Names::region_id (join.region, join.index) != context_.spot_id ())
            throw std::invalid_argument ("Region identity mismatch");
        co_return positions.admit (std::string (id), config_.Capacity)
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject (
              RegionFull{Names::region_full, join.region, join.index});
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await broadcast (); }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        positions.remove (std::string (actor.context ().actor_id ().value ()));
        co_await broadcast ();
    }
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void>
    enter (Player &actor, fx::message_context_t &, const EnterRegionRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> show (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        positions.move (std::string (actor.context ().actor_id ().value ()), request);
        co_await broadcast (&actor);
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const RegionView &update)
    {
        co_await actor.context ().bound_session ().send (update).async ();
    }
    fx::task_t<void>
    dungeon (Player &actor, fx::message_context_t &, const OpenDungeonRequest &request)
    {
        if (request.dungeonId.find_first_not_of (" \t\r\n") == std::string::npos)
            throw std::invalid_argument ("Dungeon ID required");
        const auto created = co_await spots_.get_or_create (request.dungeonId, Names::dungeon)
                               .in_mesh (Names::mesh)
                               .async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto opened =
          co_await context_.request_to_spot (request.dungeonId, DungeonInfoRequest{})
            .async<DungeonOpened> ();
        co_await actor.context ().bound_session ().send (opened).async ();
    }
};
class RegionRelocation final : public fx::spot_relocation_adapter_t<Region>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Region &region, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (region.positions);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Region &region, std::vector<std::byte> payload, std::stop_token) override
    {
        region.positions =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<Positions> ();
        co_return;
    }
};
}
